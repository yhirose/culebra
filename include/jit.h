#pragma once

#ifdef CULEBRA_JIT_ENABLED

#include <module_loader.h>
#include <parser.h>
#include <runtime/rt_macros.h>
#include <shared.h>
#include <tensor.h>
#include <unicodelib.h>
#include <unicodelib_encodings.h>

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Target/TargetMachine.h"

#include <cassert>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace culebra {

// Object property layout description, V8/SpiderMonkey "hidden class"
// style. Each Shape is interned in the process-wide ShapeRegistry;
// JitObjects with the same property set share the same Shape* and a
// fixed-offset slots array, replacing the per-instance hash/vector
// of (name, value) pairs. This is what brings "Python __slots__"
// semantics to plain Culebra Objects (and class instances, since
// `class` desugars to Object).
//
// Shapes are immutable. Adding a property to an Object transitions
// it to a new Shape via `transition_add`; the source Shape caches
// the transition so identical (source, name) pairs always resolve
// to the same target Shape. Property reads use a linear scan over
// `names` to translate name -> slot index — for the typical 5–15
// property count seen in this codebase a flat scan beats both
// std::map (tree pointer chasing) and unordered_map (hash + bucket)
// on cache traffic. The slow path is hit only on inline-cache miss.
struct Shape {
  std::vector<std::string> names;          // insertion order
  std::map<std::string_view, Shape*> add_transitions;

  bool has(std::string_view name) const {
    return offset(name) != static_cast<size_t>(-1);
  }
  // Position of `name` in `slots`, or static_cast<size_t>(-1) if absent.
  size_t offset(std::string_view name) const {
    for (size_t i = 0; i < names.size(); i++) {
      if (names[i] == name) return i;
    }
    return static_cast<size_t>(-1);
  }
};

// Process-wide intern table. Shapes live for the full program;
// the set is bounded by the number of distinct property-name sets
// the program ever uses (typically tiny — every class instance
// shares one shape, every {a, b, c} object literal shares one).
struct ShapeRegistry {
  static ShapeRegistry& instance() {
    return culebra::runtime_substate<ShapeRegistry>(culebra::kSlotShapeRegistry);
  }
  Shape* root() { return root_.get(); }

  // Return the Shape obtained by adding `name` to `current`'s
  // property set. Cached on `current->add_transitions` so identical
  // transitions collide on the same Shape pointer.
  Shape* transition_add(Shape* current, std::string_view name) {
    auto it = current->add_transitions.find(name);
    if (it != current->add_transitions.end()) return it->second;
    auto next = std::make_unique<Shape>();
    next->names = current->names;
    next->names.push_back(std::string(name));
    auto& stored_name = next->names.back();
    auto* raw = next.get();
    owned_.push_back(std::move(next));
    // Key the cache by the new shape's stored name view so the
    // string_view stays live for the lifetime of the Shape.
    current->add_transitions[std::string_view(stored_name)] = raw;
    return raw;
  }

  ShapeRegistry() : root_(std::make_unique<Shape>()) {}

 private:
  std::unique_ptr<Shape> root_;
  std::vector<std::unique_ptr<Shape>> owned_;  // keeps non-root shapes alive
};

inline ShapeRegistry& shape_registry() { return ShapeRegistry::instance(); }

}  // namespace culebra

// ---------------------------------------------------------------------------
// Runtime types and functions callable from JIT'd code
//
// THREAD SAFETY: the Culebra runtime is single-threaded. The globals
// below (exception carriers `culebra_thrown_tag` / `_data` /
// `culebra_is_throw`, the defer stack `_culebra_defer_stack`, the
// shared `InterpGC` used by the tree interpreter) are plain globals,
// NOT `thread_local`. Concurrent use from multiple host threads would
// race on these. Script-level concurrency would require moving this
// state to `thread_local` or per-interpreter-context storage.
// ---------------------------------------------------------------------------

extern "C" {

struct JitValue {
  int8_t tag;
  int64_t data;
};

struct JitArray {
  int64_t refcount;
  size_t size;
  size_t capacity;
  JitValue* items;
  // Index into the GC tracker's young vector while this Array is in
  // the young generation; -1 when in old (or untracked / shutdown).
  // Trailing field so the established IR layout for `JitArray` (which
  // codegen accesses by GEP index) is undisturbed.
  int64_t gc_slot = -1;
};

// Refcounted Tensor handle for the JIT runtime. `impl` is a shared_ptr
// so a graph node's `inputs` (also shared_ptr<TensorImpl>) and the JIT
// handle can co-own a TensorImpl across both interp and JIT paths.
// JIT-emitted IR only GEPs refcount/gc_slot — `impl` is touched from
// C++ runtime fns only.
struct JitTensor {
  int64_t refcount;
  int64_t gc_slot = -1;
  culebra::TensorPtr impl;
};

}  // extern "C"

struct JitClosure;

// Fast-path iterator advance: writes the next value into out-params
// instead of allocating a `{done, value}` step Object. Set on wrapper
// Objects by JIT-controlled iterator factories; `_iter_advance_raw`
// picks it up and skips the user-visible `next()` closure when present.
using JitIterFastFn = void (*)(JitClosure* /*next_cls*/, JitValue /*iter*/,
                               bool* /*done*/, int8_t* /*out_tag*/,
                               int64_t* /*out_data*/);

struct JitObjectEntry {
  JitValue value;
  bool mut;
};

// All refcounted heap types share the same first field: i64 refcount.
// This uniform layout lets the cycle collector read/write refcounts without
// per-type dispatch.

struct JitObject {
  int64_t refcount;
  bool has_drop = false;
  JitIterFastFn fast_next_fn = nullptr;
  // Optional prototype pointer. When set, property lookup falls through
  // to `proto->slots` after this object's own slots are exhausted (one
  // level only — proto chains aren't supported). Class-sugar instances
  // share their special methods through a per-class meta object held
  // here, so each instance's own slots carry only data fields.
  JitObject* proto = nullptr;
  // Property layout: `shape` is a process-interned descriptor mapping
  // names to slot indices; `slots[i]` holds the entry for `shape->names[i]`.
  // Two instances with the same property set share the same Shape*.
  // This is the JIT analogue of Python's __slots__ — fixed-offset
  // attribute access in place of a per-instance hash/vector lookup.
  // Shapes are immutable; adding a property transitions to a new
  // Shape via `ShapeRegistry::transition_add` (cached, so identical
  // transitions always resolve to the same target Shape).
  culebra::Shape* shape = nullptr;
  std::vector<JitObjectEntry> slots;
  // Sidecar for non-String literal keys (Phase 7-C). Lazy-allocated;
  // small objects pay nothing. Values live here; insertion order is
  // tracked in the unified `key_order` below.
  struct AnyKeyMap;  // forward decl; full definition below
  AnyKeyMap* non_string_props = nullptr;
  // Unified key insertion order — every key (String and non-String)
  // in the order it was first set. String keys are stored as
  // TAG_STRING JitValues pointing into the shared shape name pool
  // (no retain). Non-String keys are stored as their literal
  // JitValue; Tuple keys hold a +1 retain so the cycle GC and
  // destruction paths can release them via this vector. The shape
  // (for String) and `non_string_props` (for non-String) remain for
  // O(1) lookup.
  std::vector<JitValue>* key_order = nullptr;
  int64_t gc_slot = -1;  // see JitArray::gc_slot
  // Bumps on add/delete (not value updates). object_iter snapshots
  // this and fails-fast on per-step mismatch — matches Python dict
  // semantics. Trailing field so existing JitObject IR layout is
  // undisturbed.
  int64_t mut_count = 0;

  // --- Shape-based property access helpers ---

  // Slot index for `key`, or static_cast<size_t>(-1) if absent.
  size_t find_slot(std::string_view key) const {
    if (!shape) return static_cast<size_t>(-1);
    return shape->offset(key);
  }
  bool has_own(std::string_view key) const {
    return shape && shape->has(key);
  }
  size_t prop_size() const { return slots.size(); }

  // Append a new property. Caller must ensure `key` is absent;
  // otherwise the shape transition produces a corrupted layout.
  //
  // Reserves 8 slots on the first append: most JitObjects in this
  // codebase end up with ≤ 8 own properties (microgpt's Value class
  // has 6, plain Object literals 2–5, iterator wrappers 2). The
  // reserve avoids the std::vector doubling chain (cap 1→2→4→8),
  // turning 4 small heap allocations into one.
  size_t append_slot(std::string_view key, JitValue value, bool mut) {
    if (!shape) shape = culebra::shape_registry().root();
    shape = culebra::shape_registry().transition_add(shape, key);
    if (slots.capacity() == 0) slots.reserve(8);
    slots.push_back({value, mut});
    ++mut_count;
    // `key_order` is lazy: String-only objects recover insertion
    // order from `shape->names` directly at str() time. Only after a
    // non-String key activates the vector do String inserts also push
    // here (so the interleaved order survives). Tag literal 4 matches
    // TAG_STRING (defined further down).
    if (key_order) {
      auto* name_cstr = shape->names.back().c_str();
      key_order->push_back(
          {/*TAG_STRING*/ 4, reinterpret_cast<int64_t>(name_cstr)});
    }
    return slots.size() - 1;
  }

  // Set or update. Returns slot index.
  size_t set_or_append(std::string_view key, JitValue value, bool mut) {
    auto idx = find_slot(key);
    if (idx == static_cast<size_t>(-1)) {
      return append_slot(key, value, mut);
    }
    slots[idx] = {value, mut};
    return idx;
  }

  // Iterate (name, entry) pairs in insertion order.
  template <class F>
  void for_each(F&& f) const {
    if (!shape) return;
    for (size_t i = 0; i < shape->names.size(); i++) {
      f(std::string_view(shape->names[i]), slots[i]);
    }
  }
  template <class F>
  void for_each_mut(F&& f) {
    if (!shape) return;
    for (size_t i = 0; i < shape->names.size(); i++) {
      f(std::string_view(shape->names[i]), slots[i]);
    }
  }

  // Remove a property by rebuilding the shape from root. Slow path
  // (`Object.remove` is rare); cycle members invoked via the cycle
  // collector use the unified release path which clears slots wholesale
  // instead.
  void erase(std::string_view key) {
    auto idx = find_slot(key);
    if (idx == static_cast<size_t>(-1)) return;
    auto* new_shape = culebra::shape_registry().root();
    std::vector<JitObjectEntry> new_slots;
    new_slots.reserve(slots.size() - 1);
    for (size_t i = 0; i < shape->names.size(); i++) {
      if (i == idx) continue;
      new_shape = culebra::shape_registry().transition_add(
          new_shape, shape->names[i]);
      new_slots.push_back(std::move(slots[i]));
    }
    shape = new_shape;
    slots = std::move(new_slots);
    ++mut_count;
  }
};

// Per-callsite inline cache for `obj.prop` reads. Allocated as a JIT
// module global per property-get site; the slow path (object_get_ic)
// fills it on first miss and on any shape transition. Fast path IR
// inlines: load obj->shape, compare with cached_shape; on hit, load
// slots[cached_offset] directly without a runtime call.
struct JitPropIC {
  void* shape;     // last-seen Shape* (opaque to JIT IR)
  uint64_t offset; // slot index into `slots` for that shape
};

// Per-callsite inline cache for `obj.prop = ...` writes. Two modes:
//   update    — `expected == result`; `obj.prop` already exists at
//               slot `offset` and we overwrite it.
//   transition — `expected != result`; this site appends the new prop,
//               so `obj` must currently have shape `expected` and we
//               grow it to `result` (offset is implicit but stored).
// Fast path IR inlines a single shape compare; on match, it dispatches
// to a small fast helper (`object_set_fast`) that handles both modes
// without the `find_slot` linear scan or `transition_add` lookup. On
// miss, the slow helper (`object_set_ic`) does the full work and fills
// the IC.
struct JitPropSetIC {
  void* expected_shape;  // shape `obj` must have for the fast path
  void* result_shape;    // shape after this write (== expected for update)
  uint64_t offset;       // slot to write (slots[offset] for update;
                         // equals expected->names.size() for transition)
  uint8_t prop_mut;      // `mut` flag for the new entry (transition only)
};


struct JitCell {
  int64_t refcount;
  JitValue value;
  int64_t gc_slot = -1;  // see JitArray::gc_slot
};

struct JitClosure {
  int64_t refcount;
  void* fn_ptr;
  size_t n_captures;
  JitCell** captures;
  size_t arity;  // number of user-visible params (excluding __cls__, this)
  int64_t gc_slot = -1;  // see JitArray::gc_slot
};

// Uniform calling convention for every JIT closure. Callers build a
// stack slab of user args and pass (cls, this, n_args, args_ptr); the
// callee extracts its declared params from `args` and bundles any
// overflow into the function-scope `__ARGS__` Array.
using JitFn =
    JitValue (*)(JitClosure*, JitValue /*this*/, int64_t /*n_args*/,
                 JitValue* /*args*/);

// Invocation helpers for the common runtime-callback arities. Each
// packs its args into a stack array and hands them to the closure's
// fn_ptr — matching what the JIT-compiled caller emits inline. Caller
// is responsible for retaining each arg before handoff; the callee
// frame takes over ownership on entry.
inline JitValue _culebra_invoke0(JitClosure* fn) {
  return reinterpret_cast<JitFn>(fn->fn_ptr)(fn, {0, 0}, 0, nullptr);
}
inline JitValue _culebra_invoke1(JitClosure* fn, JitValue a) {
  JitValue args[1] = {a};
  return reinterpret_cast<JitFn>(fn->fn_ptr)(fn, {0, 0}, 1, args);
}
inline JitValue _culebra_invoke2(JitClosure* fn, JitValue a, JitValue b) {
  JitValue args[2] = {a, b};
  return reinterpret_cast<JitFn>(fn->fn_ptr)(fn, {0, 0}, 2, args);
}

// --- Cycle collector ---
//
// Python-style mark-and-sweep: tracks all refcounted heap objects, runs
// periodically and on program exit.

inline void _culebra_value_release_impl(int8_t tag, int64_t data);
inline void _culebra_cell_release(JitCell* c);
inline void _culebra_call_drop_if_present(JitObject* o);

// Forward decls for the refcount helpers — `culebra_runtime_module_*`
// is emitted before the helper definitions further down (line 1430-ish)
// because the runtime header is one big TU and the module table sits
// near the throw helpers.
extern "C" {
CULEBRA_RT_KEEP void culebra_runtime_value_retain(int8_t tag, int64_t data);
CULEBRA_RT_KEEP void culebra_runtime_value_release(int8_t tag, int64_t data);
}

// JitValue::tag values. The refcounted subset (Func/Array/Object)
// doubles as GC tracker tags; cells get a distinct GC-only tag below.
static constexpr int8_t TAG_NIL = 0;
static constexpr int8_t TAG_BOOL = 1;
static constexpr int8_t TAG_LONG = 2;
static constexpr int8_t TAG_FUNC = 3;
static constexpr int8_t TAG_STRING = 4;
static constexpr int8_t TAG_ARRAY = 5;
static constexpr int8_t TAG_OBJECT = 6;
// IEEE 754 binary64. The i64 `data` slot carries the bit pattern of
// the double; consumers bitcast back (see `make_float` /
// `coerce_to_double`). Float is a value type: retain/release is a
// no-op and the cycle collector ignores it via `is_refcounted_value_tag`.
static constexpr int8_t TAG_FLOAT = 7;
static constexpr int8_t TAG_TENSOR = 8;
// Immutable, ordered, hashable sequence. Storage shares JitArray's
// layout — the only runtime difference is the tag (which forbids
// mutation and unlocks use as an Object/Set key). Treating Tuple as a
// frozen Array avoids a parallel struct and per-site GEP layout fork.
static constexpr int8_t TAG_TUPLE = 9;
// Insertion-ordered unique-membership collection. Backed by a custom
// JitSet struct (parallel to JitArray) with an O(1) sidecar index.
static constexpr int8_t TAG_SET = 10;
// Borrowed-bytes view over an owning TAG_STRING (or any other char
// buffer the runtime keeps alive for the duration of use). data points
// at a heap-allocated `JitStringView { const char* ptr; uint64_t len }`
// struct; the struct itself leaks for the program's lifetime (cycle-
// bounded leak, identical to TAG_STRING char arrays). Refcounting is
// a no-op because lifetime is governed by the owning String, not by
// the view. See [[project_string_model]].
static constexpr int8_t TAG_STRINGVIEW = 11;
// Sentinel tag used in the kwargs-resolved slab to mark a middle gap
// (a defaulted slot that the caller did not fill while a later slot
// is filled). The callee prologue treats it as "fall back to default";
// it is NOT a valid value tag and must never escape into user values.
static constexpr int8_t TAG_UNFILLED = 127;

static constexpr int8_t GC_TAG_FUNC = TAG_FUNC;
static constexpr int8_t GC_TAG_ARRAY = TAG_ARRAY;
static constexpr int8_t GC_TAG_OBJECT = TAG_OBJECT;
static constexpr int8_t GC_TAG_TENSOR = TAG_TENSOR;
static constexpr int8_t GC_TAG_TUPLE = TAG_TUPLE;
static constexpr int8_t GC_TAG_SET = TAG_SET;
static constexpr int8_t GC_TAG_CELL = 100;

// Forward decl: byte-view of a TAG_STRING or TAG_STRINGVIEW payload.
inline std::string_view _culebra_str_view(int8_t tag, int64_t data);

// Forward decls: Object-side `hash()` / `eq()` dispatch used by
// JitValueHash / JitValueEq. Defined further down once `_try_special_unary`
// and `_culebra_invoke_method1` are complete (inside the runtime's
// extern "C" block — match the linkage here). Returning nullopt means
// "no user method, fall back to reference identity" so existing Object
// keys without `hash()` keep their previous (identity-based) behavior.
extern "C" {
inline std::optional<int64_t> _jit_object_user_hash(JitObject* obj);
inline std::optional<bool> _jit_object_user_eq(JitObject* a, JitObject* b);
}

// Hash/eq for JitValue keys in JitObject's non-String key sidecar.
// Numerically-equal Long/Float/Bool share a bucket (Python convention).
// Object keys route through user-defined `hash()` / `eq()` when present
// (Hashable + Eq structural conformance); otherwise reference identity.
struct JitValueHash {
  size_t operator()(const JitValue& v) const {
    switch (v.tag) {
      case TAG_NIL:  return 0;
      case TAG_BOOL: return std::hash<long>{}(v.data != 0 ? 1 : 0);
      case TAG_LONG: return std::hash<long>{}(v.data);
      case TAG_FLOAT: {
        double d;
        std::memcpy(&d, &v.data, sizeof d);
        long as_long = static_cast<long>(d);
        if (std::isfinite(d) && static_cast<double>(as_long) == d) {
          return std::hash<long>{}(as_long);
        }
        return std::hash<double>{}(d);
      }
      case TAG_STRING:
      case TAG_STRINGVIEW:
        return std::hash<std::string_view>{}(_culebra_str_view(v.tag, v.data));
      case TAG_TUPLE: {
        auto* a = reinterpret_cast<JitArray*>(v.data);
        size_t h = a->size;
        for (size_t i = 0; i < a->size; i++) {
          h = h * 31 + (*this)(a->items[i]);
        }
        return h;
      }
      case TAG_OBJECT: {
        auto* obj = reinterpret_cast<JitObject*>(v.data);
        if (auto h = _jit_object_user_hash(obj)) {
          return std::hash<int64_t>{}(*h);
        }
        throw culebra::CulebraError(
            "TypeError",
            "unhashable type: 'Object' (no hash() method)");
      }
    }
    return std::hash<int64_t>{}(v.data) ^ static_cast<size_t>(v.tag);
  }
};
struct JitValueEq {
  bool operator()(const JitValue& a, const JitValue& b) const {
    if ((a.tag == TAG_STRING || a.tag == TAG_STRINGVIEW) &&
        (b.tag == TAG_STRING || b.tag == TAG_STRINGVIEW)) {
      return _culebra_str_view(a.tag, a.data) ==
             _culebra_str_view(b.tag, b.data);
    }
    if (a.tag == b.tag) {
      if (a.tag == TAG_TUPLE) {
        auto* aa = reinterpret_cast<JitArray*>(a.data);
        auto* bb = reinterpret_cast<JitArray*>(b.data);
        if (aa == bb) return true;
        if (aa->size != bb->size) return false;
        for (size_t i = 0; i < aa->size; i++) {
          if (!(*this)(aa->items[i], bb->items[i])) return false;
        }
        return true;
      }
      if (a.tag == TAG_OBJECT) {
        if (a.data == b.data) return true;
        auto* oa = reinterpret_cast<JitObject*>(a.data);
        auto* ob = reinterpret_cast<JitObject*>(b.data);
        if (auto e = _jit_object_user_eq(oa, ob)) return *e;
        return false;
      }
      return a.data == b.data;
    }
    auto as_double = [](const JitValue& v) -> double {
      if (v.tag == TAG_LONG) return static_cast<double>(v.data);
      if (v.tag == TAG_BOOL) return v.data != 0 ? 1.0 : 0.0;
      if (v.tag == TAG_FLOAT) {
        double d;
        std::memcpy(&d, &v.data, sizeof d);
        return d;
      }
      return std::numeric_limits<double>::quiet_NaN();
    };
    bool num_a = (a.tag == TAG_LONG || a.tag == TAG_FLOAT || a.tag == TAG_BOOL);
    bool num_b = (b.tag == TAG_LONG || b.tag == TAG_FLOAT || b.tag == TAG_BOOL);
    return num_a && num_b && as_double(a) == as_double(b);
  }
};

struct JitObject::AnyKeyMap
    : std::unordered_map<JitValue, JitObjectEntry, JitValueHash, JitValueEq> {};

// Set storage: insertion-ordered members + an O(1) sidecar index.
// Membership/equality/hash use JitValueHash/JitValueEq (Python-style
// numeric-key equivalence). Mirrors the interpreter's SetValue layout.
struct JitSetIndex
    : std::unordered_set<JitValue, JitValueHash, JitValueEq> {};

struct JitSet {
  int64_t refcount;
  std::vector<JitValue> members;
  JitSetIndex* index = nullptr;
  int64_t gc_slot = -1;
};

// Minimum collect-trigger threshold; adaptive at runtime — see collect().
static constexpr size_t GC_THRESHOLD = 10000;
static constexpr int64_t GC_REFCOUNT_BOOST = 1000000;

// Type-dispatched accessor to a refcounted heap object's `gc_slot`
// field. Each tracked struct (JitArray, JitObject, JitCell,
// JitClosure) carries an i64 `gc_slot` that is the object's index
// into the tracker's young vector (or -1 when in old / untracked).
inline int64_t& _gc_slot_of(void* ptr, int8_t tag) {
  switch (tag) {
    case GC_TAG_ARRAY:
    case GC_TAG_TUPLE:  return static_cast<JitArray*>(ptr)->gc_slot;
    case GC_TAG_OBJECT: return static_cast<JitObject*>(ptr)->gc_slot;
    case GC_TAG_TENSOR: return static_cast<JitTensor*>(ptr)->gc_slot;
    case GC_TAG_SET:    return static_cast<JitSet*>(ptr)->gc_slot;
    case GC_TAG_CELL: return static_cast<JitCell*>(ptr)->gc_slot;
    default: return static_cast<JitClosure*>(ptr)->gc_slot;  // GC_TAG_FUNC
  }
}

struct _GcTracker {
  using TaggedPtr = std::pair<void*, int8_t>;

  // Generational tracker. `young` holds freshly-allocated heap
  // objects in a flat vector; each object stores its slot index in
  // its own `gc_slot` field so add and remove are O(1) push /
  // swap-pop with no per-call heap allocation. After a collect, the
  // surviving young objects are "promoted" simply by clearing their
  // gc_slot to -1 — they're no longer in any tracker. Refcount is
  // sufficient to manage their lifetime; cycles among promoted
  // objects are not detected.
  //
  // For Culebra's class-sugar workloads (params + class meta +
  // per-step intermediates) cycles among promoted objects don't
  // form in practice. Programs that mix long-lived data with cyclic
  // references will leak; that's an acceptable trade for the
  // ~30%-of-self-time we save vs the previous unordered_map<void*,
  // int8_t> for the old generation.
  std::vector<TaggedPtr> young;
  std::vector<size_t> free_slots;
  size_t alloc_counter = 0;
  size_t threshold = GC_THRESHOLD;
  bool running = false;      // prevent re-entry

  static _GcTracker& instance() {
    return culebra::runtime_substate<_GcTracker>(culebra::kSlotJitGc);
  }

  _GcTracker() {
    young.reserve(1 << 18);
    free_slots.reserve(1 << 14);
  }

  ~_GcTracker() { collect(); }

  void add(void* ptr, int8_t tag) {
    size_t slot;
    if (!free_slots.empty()) {
      slot = free_slots.back();
      free_slots.pop_back();
      young[slot] = {ptr, tag};
    } else {
      slot = young.size();
      young.push_back({ptr, tag});
    }
    _gc_slot_of(ptr, tag) = static_cast<int64_t>(slot);
    if (!running && ++alloc_counter >= threshold) {
      alloc_counter = 0;
      collect();
    }
  }
  void remove(void* ptr, int8_t tag) {
    auto& slot = _gc_slot_of(ptr, tag);
    if (slot >= 0) {
      young[slot] = {nullptr, 0};
      free_slots.push_back(static_cast<size_t>(slot));
      slot = -1;
    }
    // gc_slot == -1: already promoted (or untracked) — nothing to do.
  }

  static bool is_refcounted_value_tag(int8_t tag) {
    return tag == GC_TAG_FUNC || tag == GC_TAG_ARRAY ||
           tag == GC_TAG_OBJECT || tag == GC_TAG_TENSOR ||
           tag == GC_TAG_TUPLE || tag == GC_TAG_SET;
  }

  static void push_if_refcounted(std::vector<TaggedPtr>& out, int8_t tag,
                                 int64_t data) {
    if (is_refcounted_value_tag(tag))
      out.push_back({reinterpret_cast<void*>(data), tag});
  }

  // Enumerate child objects (cells/closures/arrays/objects) directly
  // reachable from ptr. Tags travel with pointers so the cycle GC can
  // call `_gc_slot_of(ptr, tag)` directly on each child.
  void enumerate_children(void* ptr, int8_t tag,
                          std::vector<TaggedPtr>& out) {
    switch (tag) {
      case GC_TAG_FUNC: {
        auto* c = static_cast<JitClosure*>(ptr);
        for (size_t i = 0; i < c->n_captures; i++) {
          if (c->captures[i]) out.push_back({c->captures[i], GC_TAG_CELL});
        }
        break;
      }
      case GC_TAG_ARRAY:
      case GC_TAG_TUPLE: {
        auto* a = static_cast<JitArray*>(ptr);
        for (size_t i = 0; i < a->size; i++) {
          push_if_refcounted(out, a->items[i].tag, a->items[i].data);
        }
        break;
      }
      case GC_TAG_SET: {
        auto* s = static_cast<JitSet*>(ptr);
        for (auto& m : s->members) {
          push_if_refcounted(out, m.tag, m.data);
        }
        break;
      }
      case GC_TAG_OBJECT: {
        auto* o = static_cast<JitObject*>(ptr);
        for (auto& entry : o->slots) {
          push_if_refcounted(out, entry.value.tag, entry.value.data);
        }
        // Non-String keys (Tuple) and their values: both can be
        // refcounted. Walk `key_order` for the keys; String entries
        // are filtered by `push_if_refcounted` since TAG_STRING isn't
        // a refcounted tag.
        if (o->key_order && o->non_string_props) {
          for (const auto& k : *o->key_order) {
            push_if_refcounted(out, k.tag, k.data);
            if (k.tag == TAG_STRING) continue;
            auto it = o->non_string_props->find(k);
            if (it != o->non_string_props->end()) {
              push_if_refcounted(out, it->second.value.tag,
                                 it->second.value.data);
            }
          }
        }
        // Reach the shared class meta via proto so GC sees it as live
        // while any instance is alive (and discovers methods through
        // it on subsequent walks).
        if (o->proto) out.push_back({o->proto, GC_TAG_OBJECT});
        break;
      }
      case GC_TAG_CELL: {
        auto* cell = static_cast<JitCell*>(ptr);
        push_if_refcounted(out, cell->value.tag, cell->value.data);
        break;
      }
    }
  }

  // All refcounted types share refcount as their first i64 field.
  static int64_t& refcount_ref(void* ptr) {
    return *reinterpret_cast<int64_t*>(ptr);
  }

  // Clear internal references by releasing each child.
  // This breaks cycles and cascades normal RC freeing.
  void clear_references(void* ptr, int8_t tag) {
    switch (tag) {
      case GC_TAG_FUNC: {
        auto* c = static_cast<JitClosure*>(ptr);
        for (size_t i = 0; i < c->n_captures; i++) {
          if (c->captures[i]) {
            auto* cell = c->captures[i];
            c->captures[i] = nullptr;
            _culebra_cell_release(cell);
          }
        }
        break;
      }
      case GC_TAG_ARRAY:
      case GC_TAG_TUPLE: {
        auto* a = static_cast<JitArray*>(ptr);
        auto items = a->items;
        auto size = a->size;
        a->items = nullptr;
        a->size = 0;
        a->capacity = 0;
        for (size_t i = 0; i < size; i++) {
          _culebra_value_release_impl(items[i].tag, items[i].data);
        }
        delete[] items;
        break;
      }
      case GC_TAG_SET: {
        auto* s = static_cast<JitSet*>(ptr);
        auto members = std::move(s->members);
        s->members.clear();
        if (s->index) {
          delete s->index;
          s->index = nullptr;
        }
        for (auto& m : members) {
          _culebra_value_release_impl(m.tag, m.data);
        }
        break;
      }
      case GC_TAG_OBJECT: {
        auto* o = static_cast<JitObject*>(ptr);
        // `drop` is intentionally NOT fired for cycle members. The
        // interpreter has no cycle collector and therefore never runs
        // user drops on cycles either; firing them only in the JIT
        // would diverge observable behavior (drop side-effects:
        // puts, file close, counter decrement, ...). This matches
        // Python's __del__ rule for cyclic garbage: the collector
        // breaks the cycle but skips finalizers because their order
        // would be non-deterministic. Non-cyclic objects still hit
        // their drop via the normal _culebra_value_release_impl
        // refcount-to-zero path.
        if (o->proto) {
          auto* proto = o->proto;
          o->proto = nullptr;
          _culebra_value_release_impl(GC_TAG_OBJECT,
                                       reinterpret_cast<int64_t>(proto));
        }
        auto slots = std::move(o->slots);
        o->slots.clear();
        o->shape = nullptr;
        for (auto& entry : slots) {
          _culebra_value_release_impl(entry.value.tag, entry.value.data);
        }
        // Non-String sidecar: same teardown as the normal release path.
        if (o->key_order) {
          for (auto& k : *o->key_order) {
            // String entries are unretained shape-name pointers; only
            // refcounted keys (currently Tuple) hold a +1 here.
            if (_GcTracker::is_refcounted_value_tag(k.tag)) {
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
        break;
      }
      case GC_TAG_CELL: {
        auto* cell = static_cast<JitCell*>(ptr);
        auto v = cell->value;
        cell->value = {0, 0};
        _culebra_value_release_impl(v.tag, v.data);
        break;
      }
    }
  }

  // Minor-only generational GC: every collect scans only `young`,
  // promotes survivors to `old`, and never re-scans `old`. Cycles
  // entirely inside long-lived objects are not detected. For the
  // class-sugar workloads Culebra is targeted at (params + class
  // metadata + per-step intermediates) those cycles don't form in
  // practice. Programs that hit a real long-lived-cycle pattern
  // would need an explicit major-collect entry point — left as
  // future work; threshold-triggered major collects regressed bench
  // when tested because `old` for HOF-style microgpt grows past any
  // reasonable threshold due to closure capture patterns.
  // Alive entry count in young (excluding swap-popped null slots).
  size_t young_alive() const { return young.size() - free_slots.size(); }

  void collect() {
    if (young_alive() == 0 || running) return;
    running = true;
    _do_collect();
    threshold = std::max(GC_THRESHOLD, young_alive() * 2);
    running = false;
  }

  void _do_collect() {
    if (young_alive() == 0) return;

    if (std::getenv("CULEBRA_GC_DEBUG")) {
      std::println(stderr, "[GC] scan young={}", young_alive());
    }

    // Minor collect: snapshot young objects and run the existing
    // decrement / BFS reachability over them. Old objects are not
    // visited; if a young object is held only by an old one, it
    // shows up as a reachable root because gc_refs keeps the full
    // refcount and we never decrement for old->young edges (old
    // isn't iterated to enumerate children).
    //
    // All collect-local state is keyed by snapshot index. Pointer →
    // index lookup avoids a hash table by repurposing each tracked
    // object's `gc_slot` field as the snapshot index for the duration
    // of this collect. Promoted (or untracked) objects keep their
    // existing `gc_slot == -1`, so a child whose `_gc_slot_of()` is
    // negative is "outside the snapshot" — same semantics the prior
    // `unordered_map::find` returned.
    std::vector<TaggedPtr> snapshot;
    snapshot.reserve(young_alive());
    for (auto& e : young) {
      if (e.first) snapshot.push_back(e);
    }

    for (size_t i = 0; i < snapshot.size(); i++) {
      _gc_slot_of(snapshot[i].first, snapshot[i].second) =
          static_cast<int64_t>(i);
    }

    std::vector<int64_t> gc_refs(snapshot.size());
    for (size_t i = 0; i < snapshot.size(); i++) {
      gc_refs[i] = refcount_ref(snapshot[i].first);
    }

    // Subtract internal references.
    std::vector<TaggedPtr> children;
    for (size_t i = 0; i < snapshot.size(); i++) {
      children.clear();
      enumerate_children(snapshot[i].first, snapshot[i].second, children);
      for (auto& c : children) {
        auto idx = _gc_slot_of(c.first, c.second);
        if (idx >= 0 && static_cast<size_t>(idx) < snapshot.size()) {
          --gc_refs[idx];
        }
      }
    }

    // Mark external roots and propagate reachability via index BFS.
    std::vector<bool> reachable(snapshot.size(), false);
    std::queue<size_t> q;
    for (size_t i = 0; i < snapshot.size(); i++) {
      if (gc_refs[i] > 0) {
        reachable[i] = true;
        q.push(i);
      }
    }
    while (!q.empty()) {
      auto i = q.front();
      q.pop();
      children.clear();
      enumerate_children(snapshot[i].first, snapshot[i].second, children);
      for (auto& c : children) {
        auto idx = _gc_slot_of(c.first, c.second);
        if (idx < 0 || static_cast<size_t>(idx) >= snapshot.size()) continue;
        if (!reachable[idx]) {
          reachable[idx] = true;
          q.push(idx);
        }
      }
    }

    // Collect garbage (unreachable)
    std::vector<TaggedPtr> garbage;
    for (size_t i = 0; i < snapshot.size(); i++) {
      if (!reachable[i]) garbage.push_back(snapshot[i]);
    }

    if (!garbage.empty()) {
      if (std::getenv("CULEBRA_GC_DEBUG")) {
        std::println(stderr, "[GC] collecting {} cycle objects (tracked={})",
                     garbage.size(), snapshot.size());
      }

      // Boost refcounts so intra-cycle releases during `clear_references`
      // can't drive any garbage entry to 0 prematurely.
      for (auto& [p, t] : garbage) refcount_ref(p) += GC_REFCOUNT_BOOST;
      for (auto& [p, t] : garbage) clear_references(p, t);

      // Pre-detach each garbage entry from `young` and clear its
      // `gc_slot` so the upcoming destructor's `_gc().remove()` is a
      // no-op (it would otherwise read the snapshot index left by
      // step (1) and clear the wrong slot).
      for (auto& [p, t] : garbage) {
        auto idx = _gc_slot_of(p, t);
        young[static_cast<size_t>(idx)] = {nullptr, 0};
        free_slots.push_back(static_cast<size_t>(idx));
        _gc_slot_of(p, t) = -1;
      }

      // refcount = 1 then release → destructor runs, frees the
      // (already-emptied) children, deletes the storage.
      for (auto& [p, t] : garbage) {
        refcount_ref(p) = 1;
        if (t == GC_TAG_CELL) {
          _culebra_cell_release(static_cast<JitCell*>(p));
        } else {
          _culebra_value_release_impl(t, reinterpret_cast<int64_t>(p));
        }
      }
    }

    // Promote survivors by clearing their (still-snapshot-index)
    // `gc_slot` to -1; they leave the tracker entirely, so cycles
    // formed entirely among promoted objects won't be collected.
    for (auto& [p, t] : young) {
      if (p) _gc_slot_of(p, t) = -1;
    }
    young.clear();
    free_slots.clear();
  }
};

inline _GcTracker& _gc() { return _GcTracker::instance(); }

// Cycle detection during string conversion.
inline thread_local std::unordered_set<const void*> _jit_str_visiting;

// RAII: inserts on construction, erases on destruction. `already` is true if
// the pointer was present (i.e., we're inside a cycle).
struct _JitStrGuard {
  const void* key;
  bool already;
  explicit _JitStrGuard(const void* k) : key(k) {
    already = !_jit_str_visiting.insert(k).second;
  }
  ~_JitStrGuard() {
    if (!already) _jit_str_visiting.erase(key);
  }
};

// Heap-allocated descriptor for TAG_STRINGVIEW. Bytes are owned
// elsewhere (leak-bounded, same as TAG_STRING).
struct JitStringView {
  const char* ptr;
  uint64_t len;
};
static_assert(sizeof(JitStringView) == 16, "JitStringView layout drift");

// Internal helper (C++ - not extern C, but inline to satisfy ODR)
inline std::string _culebra_value_to_str_impl(int8_t type, int64_t data) {
  switch (type) {
    case TAG_NIL:
      return "nil";
    case TAG_BOOL:
      return data ? "true" : "false";
    case TAG_LONG:
      return std::to_string(data);
    case TAG_FUNC:
      return "[function]";
    case TAG_STRING: {
      std::string s = "'";
      s += reinterpret_cast<const char*>(data);
      s += "'";
      return s;
    }
    case TAG_STRINGVIEW: {
      auto* v = reinterpret_cast<const JitStringView*>(data);
      std::string s = "'";
      s.append(v->ptr, v->len);
      s += "'";
      return s;
    }
    case TAG_FLOAT: {
      double d;
      std::memcpy(&d, &data, sizeof(d));
      return culebra::format_float_shortest(d);
    }
    case TAG_ARRAY: {
      auto* arr = reinterpret_cast<JitArray*>(data);
      _JitStrGuard guard(arr);
      if (guard.already) return "[...]";
      std::string s = "[";
      for (size_t i = 0; i < arr->size; i++) {
        if (i > 0) s += ", ";
        s += _culebra_value_to_str_impl(arr->items[i].tag,
                                        arr->items[i].data);
      }
      s += "]";
      return s;
    }
    case TAG_TUPLE: {
      auto* arr = reinterpret_cast<JitArray*>(data);
      _JitStrGuard guard(arr);
      if (guard.already) return "(...)";
      std::string s = "(";
      for (size_t i = 0; i < arr->size; i++) {
        if (i > 0) s += ", ";
        s += _culebra_value_to_str_impl(arr->items[i].tag,
                                        arr->items[i].data);
      }
      // (x,) trailing comma marks the 1-element tuple, matching the
      // interpreter and Python.
      if (arr->size == 1) s += ",";
      s += ")";
      return s;
    }
    case TAG_SET: {
      auto* set = reinterpret_cast<JitSet*>(data);
      _JitStrGuard guard(set);
      if (guard.already) return "{...}";
      std::string s = "{";
      for (size_t i = 0; i < set->members.size(); i++) {
        if (i > 0) s += ", ";
        s += _culebra_value_to_str_impl(set->members[i].tag,
                                        set->members[i].data);
      }
      s += "}";
      return s;
    }
#ifndef CULEBRA_RT_NO_TENSOR
    case TAG_TENSOR: {
      auto* t = reinterpret_cast<JitTensor*>(data);
      return tensor_str(*t->impl);
    }
#endif
    case TAG_OBJECT: {
      auto* obj = reinterpret_cast<JitObject*>(data);
      _JitStrGuard guard(obj);
      if (guard.already) return "{...}";
      // Hoist a String `class:` tag to a prefix (matches the tree
      // interpreter's str_object formatting).
      std::string s;
      bool has_class_tag = false;
      {
        auto idx = obj->find_slot("class");
        if (idx != static_cast<size_t>(-1) &&
            obj->slots[idx].value.tag == TAG_STRING) {
          s = reinterpret_cast<const char*>(obj->slots[idx].value.data);
          s += " ";
          has_class_tag = true;
        }
      }
      s += "{";
      bool first = true;
      if (obj->key_order) {
        // Mixed-key path: walk the unified vector so String and
        // non-String keys interleave in true insertion order.
        for (const auto& key : *obj->key_order) {
          if (key.tag == TAG_STRING) {
            auto name = reinterpret_cast<const char*>(key.data);
            if (has_class_tag && std::string_view(name) == "class") continue;
            auto idx = obj->shape ? obj->shape->offset(name)
                                  : static_cast<size_t>(-1);
            if (idx == static_cast<size_t>(-1)) continue;
            const auto& entry = obj->slots[idx];
            if (!first) s += ", ";
            first = false;
            if (entry.mut) s += "mut ";
            s += name;
            s += ": ";
            s += _culebra_value_to_str_impl(entry.value.tag,
                                            entry.value.data);
          } else {
            if (!obj->non_string_props) continue;
            auto it = obj->non_string_props->find(key);
            if (it == obj->non_string_props->end()) continue;
            if (!first) s += ", ";
            first = false;
            if (it->second.mut) s += "mut ";
            s += _culebra_value_to_str_impl(key.tag, key.data);
            s += ": ";
            s += _culebra_value_to_str_impl(it->second.value.tag,
                                            it->second.value.data);
          }
        }
      } else if (obj->shape) {
        // String-only fast path: walk shape->names directly, skipping
        // the per-property key_order push that mixed-key objects need.
        for (size_t i = 0; i < obj->shape->names.size(); i++) {
          const auto& name = obj->shape->names[i];
          if (has_class_tag && name == "class") continue;
          const auto& entry = obj->slots[i];
          if (!first) s += ", ";
          first = false;
          if (entry.mut) s += "mut ";
          s += name;
          s += ": ";
          s += _culebra_value_to_str_impl(entry.value.tag,
                                          entry.value.data);
        }
      }
      s += "}";
      return s;
    }
    default:
      return "[unknown]";
  }
}

inline const char* _culebra_heap_str(std::string_view s) {
  auto buf = static_cast<char*>(std::malloc(s.size() + 1));
  std::memcpy(buf, s.data(), s.size());
  buf[s.size()] = '\0';
  return buf;
}

// Allocate a JitStringView descriptor for the given byte range.
// Leaks for the program's lifetime (cycle-bounded, same as
// _culebra_heap_str). Callers retain no ownership beyond storing
// the returned pointer in a JitValue.
inline JitStringView* _culebra_heap_view(const char* ptr, uint64_t len) {
  auto* v = static_cast<JitStringView*>(std::malloc(sizeof(JitStringView)));
  v->ptr = ptr;
  v->len = len;
  return v;
}

// Adapter: get a std::string_view over the bytes of either a TAG_STRING
// (data is `const char*`, null-terminated) or a TAG_STRINGVIEW (data is
// `JitStringView*`). Other tags return an empty view — callers should
// pre-check the tag.
inline std::string_view _culebra_str_view(int8_t tag, int64_t data) {
  if (tag == TAG_STRING) {
    auto* s = reinterpret_cast<const char*>(data);
    return std::string_view(s);
  }
  if (tag == TAG_STRINGVIEW) {
    auto* v = reinterpret_cast<const JitStringView*>(data);
    return std::string_view(v->ptr, v->len);
  }
  return {};
}

// Bitcast the i64 payload of a Float JitValue back to double.
inline double _culebra_float_to_double(int64_t data) {
  double d;
  std::memcpy(&d, &data, sizeof(d));
  return d;
}

inline int64_t _culebra_double_to_bits(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i;
}

// Coerce a numeric JitValue (Long or Float) to double. Any other tag
// throws. Used by the arithmetic/comparison slow paths.
inline double _culebra_coerce_num(int8_t tag, int64_t data) {
  if (tag == TAG_LONG) return static_cast<double>(data);
  if (tag == TAG_FLOAT) return _culebra_float_to_double(data);
  throw culebra::CulebraError("TypeError", "type error.");
}

// Same-tag equality matching the interpreter's operator==. Strings compare by
// contents; reference types (func/array/object) by identity. Long↔Float
// cross-type uses numeric equality (`1 == 1.0`).
inline bool _culebra_value_equal(int8_t t1, int64_t d1, int8_t t2, int64_t d2) {
  if (t1 != t2) {
    // Numeric cross-type: promote to double and compare by value.
    if ((t1 == TAG_LONG || t1 == TAG_FLOAT) &&
        (t2 == TAG_LONG || t2 == TAG_FLOAT)) {
      return _culebra_coerce_num(t1, d1) == _culebra_coerce_num(t2, d2);
    }
    // String/StringView byte-equality across flavors.
    if ((t1 == TAG_STRING || t1 == TAG_STRINGVIEW) &&
        (t2 == TAG_STRING || t2 == TAG_STRINGVIEW)) {
      return _culebra_str_view(t1, d1) == _culebra_str_view(t2, d2);
    }
    return false;
  }
  switch (t1) {
    case TAG_NIL: return true;
    case TAG_BOOL: return (d1 != 0) == (d2 != 0);
    case TAG_LONG: return d1 == d2;
    case TAG_FLOAT:
      return _culebra_float_to_double(d1) == _culebra_float_to_double(d2);
    case TAG_STRING:
      return std::strcmp(reinterpret_cast<const char*>(d1),
                         reinterpret_cast<const char*>(d2)) == 0;
    case TAG_STRINGVIEW:
      return _culebra_str_view(t1, d1) == _culebra_str_view(t2, d2);
    case TAG_TUPLE: {
      // Element-wise eq, matching the interp's TupleValue compare.
      auto* a = reinterpret_cast<JitArray*>(d1);
      auto* b = reinterpret_cast<JitArray*>(d2);
      if (a == b) return true;
      if (a->size != b->size) return false;
      for (size_t i = 0; i < a->size; i++) {
        if (!_culebra_value_equal(a->items[i].tag, a->items[i].data,
                                  b->items[i].tag, b->items[i].data)) {
          return false;
        }
      }
      return true;
    }
    case TAG_SET: {
      // Set eq: same size and every element of `a` is present in `b`'s
      // index. Mirrors the interp's _set_eq.
      auto* a = reinterpret_cast<JitSet*>(d1);
      auto* b = reinterpret_cast<JitSet*>(d2);
      if (a == b) return true;
      if (a->members.size() != b->members.size()) return false;
      if (!b->index) return false;
      for (auto& m : a->members) {
        if (!b->index->contains(m)) return false;
      }
      return true;
    }
    default: return d1 == d2;  // func/array/object: identity
  }
}

// Ordering helpers. Each one exactly mirrors the corresponding
// interpreter `Value::operator<` / `<=` / `>` / `>=` so JIT
// semantics match bit-for-bit. Nil is a special case: `nil op nil`
// always returns false (ordering on `nil` is not defined). Cross-type
// numeric (Long↔Float) promotes to double.

template <typename Cmp>
inline bool _culebra_value_ord(int8_t t1, int64_t d1, int8_t t2, int64_t d2,
                               Cmp cmp) {
  if (t1 != t2) {
    if ((t1 == TAG_LONG || t1 == TAG_FLOAT) &&
        (t2 == TAG_LONG || t2 == TAG_FLOAT)) {
      return cmp(_culebra_coerce_num(t1, d1), _culebra_coerce_num(t2, d2));
    }
    throw culebra::CulebraError("TypeError", "type error.");
  }
  switch (t1) {
    case TAG_NIL: return false;
    case TAG_BOOL: return cmp(double(d1 != 0), double(d2 != 0));
    case TAG_LONG: return cmp(double(d1), double(d2));
    case TAG_FLOAT:
      return cmp(_culebra_float_to_double(d1), _culebra_float_to_double(d2));
    case TAG_STRING: {
      auto c = std::strcmp(reinterpret_cast<const char*>(d1),
                           reinterpret_cast<const char*>(d2));
      return cmp(double(c), 0.0);
    }
    default: throw culebra::CulebraError("TypeError", "type error.");
  }
}

// Four ordering predicates share `_culebra_value_ord`'s Nil/cross-type
// scaffolding; the public extern "C" trampolines below are the only
// callers. A single `cmp` comparator parameterises the leaf compare.

extern "C" {

// Forward decl; the `__str__` dispatcher lives alongside the other
// special-method helpers further down (it returns std::optional<std::string>,
// so it needs full C++ linkage — can't be declared inside the
// enclosing extern "C" block).
inline std::optional<std::string> _try_str_special(int8_t type, int64_t data);

// Forward decls used by `culebra_runtime_hash_any` and the
// `_jit_object_user_*` helpers (defined alongside the other special-
// method helpers further down).
inline std::optional<JitValue> _try_special_unary(int8_t t, int64_t d,
                                                  const char* name);
inline const JitObjectEntry* _find_property(JitObject* obj,
                                            const char* key);

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_puts(int8_t type,
                                                       int64_t data) {
  if (auto s = _try_str_special(type, data)) {
    std::cout << *s << std::endl;
    return;
  }
  switch (type) {
    case TAG_NIL:
      std::cout << "nil" << std::endl;
      break;
    case TAG_BOOL:
      std::cout << (data ? "true" : "false") << std::endl;
      break;
    case TAG_LONG:
      std::cout << data << std::endl;
      break;
    case TAG_STRING:
      std::cout << "'" << reinterpret_cast<const char*>(data) << "'"
                << std::endl;
      break;
    case TAG_STRINGVIEW: {
      auto* v = reinterpret_cast<const JitStringView*>(data);
      std::cout << "'";
      std::cout.write(v->ptr, static_cast<std::streamsize>(v->len));
      std::cout << "'" << std::endl;
      break;
    }
    case TAG_ARRAY:
    case TAG_OBJECT:
    case TAG_FLOAT:
    case TAG_TENSOR:
    case TAG_TUPLE:
    case TAG_SET:
      std::cout << _culebra_value_to_str_impl(type, data) << std::endl;
      break;
    default:
      std::cout << "[unknown]" << std::endl;
      break;
  }
}

// For interpolation / print / to_string: strings unquoted, Objects
// with `__str__` return their custom form.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_value_to_display(
    int8_t type, int64_t data) {
  if (auto s = _try_str_special(type, data)) return _culebra_heap_str(*s);
  if (type == TAG_STRING) return reinterpret_cast<const char*>(data);
  if (type == TAG_STRINGVIEW) {
    auto* v = reinterpret_cast<const JitStringView*>(data);
    return _culebra_heap_str(std::string_view(v->ptr, v->len));
  }
  return _culebra_heap_str(_culebra_value_to_str_impl(type, data));
}

// `hash(v)` builtin runtime entry. Routes Object to a user-defined
// `hash()` method (Hashable + Eq structural conformance); primitives
// go through JitValueHash — same path JitObject's AnyKeyMap uses.
// Throws on unhashable inputs (Array / Set / Function / Tensor, Object
// without `hash`). Returns a raw int64 (Long payload).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_hash_any(
    int8_t type, int64_t data, int64_t line, int64_t col) {
  if (type == TAG_OBJECT) {
    auto r = _try_special_unary(type, data, "hash");
    if (!r) {
      throw culebra::CulebraError(
          "TypeError",
          "unhashable type: 'Object' (no hash() method)",
          static_cast<int>(line), static_cast<int>(col));
    }
    if (r->tag != TAG_LONG) {
      _culebra_value_release_impl(r->tag, r->data);
      throw culebra::CulebraError("TypeError",
                                  "hash() must return Long",
                                  static_cast<int>(line),
                                  static_cast<int>(col));
    }
    return r->data;
  }
  // Primitives go through the same hash that JitObject's AnyKeyMap uses.
  // Unhashable inputs (Array / Set / Function / Tensor) take the default
  // fallback path (data-bit identity); the Hashable trait dispatch keeps
  // them out of `hash(v)` callers in practice.
  return static_cast<int64_t>(JitValueHash{}(JitValue{type, data}));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_concat(
    const char* a, const char* b) {
  auto la = std::strlen(a);
  auto lb = std::strlen(b);
  auto r = static_cast<char*>(std::malloc(la + lb + 1));
  std::memcpy(r, a, la);
  std::memcpy(r + la, b, lb);
  r[la + lb] = '\0';
  return r;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_str_eq(const char* a,
                                                         const char* b) {
  return std::strcmp(a, b) == 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int32_t culebra_runtime_str_cmp(const char* a,
                                                             const char* b) {
  return std::strcmp(a, b);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_type_error(int64_t line,
                                                             int64_t col) {
  culebra::throw_type_error_at(line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_destructure_mismatch(int64_t line, int64_t col) {
  culebra::throw_destructure_mismatch_at(line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_compound_missing_property(int64_t line, int64_t col) {
  culebra::throw_compound_missing_property_at(line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_immutable_assign(const char* name, int64_t line, int64_t col) {
  culebra::throw_immutable_assign_at(name ? name : "", line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_unknown_kwarg(const char* name, int64_t line, int64_t col) {
  culebra::throw_unknown_kwarg_at(name ? name : "", line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_missing_required_arg(const char* name, int64_t line, int64_t col) {
  culebra::throw_missing_required_arg_at(name ? name : "", line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_throw_error(const char* kind, const char* msg,
                             int64_t line, int64_t col) {
  culebra::throw_runtime_error_at(kind ? kind : "", msg ? msg : "",
                                   line, col);
}

// JIT module table — keyed by absolute module path, value is the
// module's export Object packed into a JitValue. Lives in the active
// Runtime so it dies with the Runtime (releasing each held +1) rather
// than persisting as thread-local state across embedding sessions.
struct _JitModuleTable {
  std::unordered_map<std::string, JitValue> entries;
  ~_JitModuleTable() {
    for (auto& [_, v] : entries) _culebra_value_release_impl(v.tag, v.data);
  }
};
inline std::unordered_map<std::string, JitValue>& _jit_module_table() {
  return culebra::runtime_substate<_JitModuleTable>(
             culebra::kSlotJitModuleTable).entries;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_module_register(const char* path, int8_t tag, int64_t data) {
  auto& t = _jit_module_table();
  auto key = std::string(path ? path : "");
  auto it = t.find(key);
  if (it != t.end()) {
    // Replace: drop the old entry's +1 before overwriting so the slot
    // never holds two refs at once.
    culebra_runtime_value_release(it->second.tag, it->second.data);
    it->second = JitValue{tag, data};
  } else {
    t.emplace(std::move(key), JitValue{tag, data});
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_module_get(const char* path, int8_t* out_tag,
                            int64_t* out_data, int64_t line, int64_t col) {
  auto& t = _jit_module_table();
  auto it = t.find(std::string(path ? path : ""));
  if (it == t.end()) {
    culebra::throw_runtime_error_at(
        "ImportError",
        std::format("module '{}' was not loaded — `import` statements "
                    "must be reachable from the entry point's "
                    "dependency graph", path ? path : ""),
        line, col);
  }
  // Hand the caller a fresh +1 so the importing scope owns its own
  // reference (table retains the original).
  culebra_runtime_value_retain(it->second.tag, it->second.data);
  *out_tag = it->second.tag;
  *out_data = it->second.data;
}

// Like type_error but includes "expected X, got Y" — caller passes the
// expected type as a string-literal global and the runtime tag of the
// actual value. Used by leaf JIT accessors (value_to_long etc.) where
// both pieces of context are statically available at the throw site.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_type_error_typed(
    int64_t line, int64_t col, const char* expected, int8_t got_tag) {
  const char* got = "?";
  switch (got_tag) {
    case TAG_NIL:    got = "Nil";      break;
    case TAG_BOOL:   got = "Bool";     break;
    case TAG_LONG:   got = "Long";     break;
    case TAG_FLOAT:  got = "Float";    break;
    case TAG_STRING: got = "String";   break;
    case TAG_ARRAY:  got = "Array";    break;
    case TAG_OBJECT: got = "Object";   break;
    case TAG_FUNC:   got = "Function"; break;
    case TAG_TENSOR: got = "Tensor";   break;
    case TAG_TUPLE:  got = "Tuple";    break;
    case TAG_SET:    got = "Set";      break;
    case TAG_STRINGVIEW: got = "StringView"; break;
  }
  throw culebra::CulebraError("TypeError", std::format(
      "type error: expected {}, got {} at {}:{}.", expected, got, line, col),
      line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_arity_error(
    int64_t got, int64_t declared, int64_t line, int64_t col) {
  // Legacy entry point (kept for ABI continuity in JIT runtimes that
  // were compiled before culebra_runtime_arity_missing existed).
  throw culebra::CulebraError("ArityError", std::format(
      "arguments error: called with {} argument(s), expected at least {} "
      "at {}:{}.",
      got, declared, line, col), line, col);
}

// Name-aware arity error: callee passes its declared parameter name
// table (a const char* array, NUL-terminated entries) and the runtime
// throws "missing required argument 'X'" where X is the first slot
// that wasn't filled. Matches interp's eval-time message exactly so
// `e.message.contains('missing required')` works on both backends.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_arity_missing(const char* const* names, int64_t got,
                               int64_t line, int64_t col) {
  const char* missing = (names && names[got]) ? names[got] : "";
  culebra::throw_missing_required_arg_at(missing, line, col);
}

// C++ exception thrown by `culebra_runtime_throw` when user code runs
// `throw expr`. The payload (tag + data) is NOT read via this object
// in JITed code — that would require exposing the typeinfo symbol
// (`_ZTI16CulebraException`) to the JIT linker, which is fragile
// across platforms. Instead, landingpads use catch-all and read the
// `culebra_thrown_*` globals below. The member fields are only read
// by the host-side catch in JIT::run to format the uncaught-throw
// message.
class CulebraException : public std::exception {
 public:
  int8_t tag;
  int64_t data;
  CulebraException(int8_t t, int64_t d) : tag(t), data(d) {}
  const char* what() const noexcept override { return "CulebraException"; }
};

CULEBRA_RT_KEEP void culebra_runtime_value_retain(int8_t tag,
                                                        int64_t data);

// Culebra is single-threaded, so plain globals are fine. Populated
// by `culebra_runtime_throw` and read by try/catch landingpads to
// distinguish user throws (`culebra_is_throw == 1`) from internal
// runtime errors (`std::runtime_error` etc., which re-raise).
// JIT IR reaches the carriers through these accessors — ORC's emutls
// can't resolve `__emutls_v.*` from JIT modules, so a regular call
// into C++ (where Runtime lookup just works) is the portable path.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_get_is_throw() {
  return culebra::current_runtime().is_throw;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_clear_is_throw() {
  culebra::current_runtime().is_throw = 0;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_get_thrown_tag() {
  return culebra::current_runtime().thrown_tag;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_get_thrown_data() {
  return culebra::current_runtime().thrown_data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_throw(int8_t tag,
                                                        int64_t data) {
  // Retain so the payload stays alive through stack unwinding. The
  // matching catch handler is responsible for the balancing release.
  culebra_runtime_value_retain(tag, data);
  auto& rt = culebra::current_runtime();
  rt.thrown_tag = tag;
  rt.thrown_data = data;
  rt.is_throw = 1;
  throw CulebraException(tag, data);
}

// Re-throw the currently-in-flight exception. Used by cleanup
// landingpads to let the exception keep unwinding after running
// deferred work, while still allowing the JIT to choose the next
// landingpad via an `invoke` unwind edge (LLVM's `resume` alone
// abandons the current function, skipping outer landingpads in the
// same function).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_rethrow() {
  throw;
}

// --- Defer stack (JIT side) ---
//
// Culebra is single-threaded, so a single global LIFO stack of retained
// closure Values is sufficient. Each lexical scope records a "mark"
// (the stack size) on entry and, on every exit path (fall-through,
// return, throw), unwinds the stack back to that mark — running each
// popped closure as a 0-arg call.
inline std::vector<JitValue>& _culebra_defer_stack() {
  return culebra::runtime_substate<std::vector<JitValue>>(
      culebra::kSlotDeferStack);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_defer_mark() {
  return static_cast<int64_t>(_culebra_defer_stack().size());
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_defer_push(int8_t tag,
                                                             int64_t data) {
  // Retain: stack owns a reference until run_to drops it.
  culebra_runtime_value_retain(tag, data);
  _culebra_defer_stack().push_back(JitValue{tag, data});
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_defer_run_to(int64_t mark) {
  auto& s = _culebra_defer_stack();
  while (static_cast<int64_t>(s.size()) > mark) {
    auto v = s.back();
    s.pop_back();
    auto* c = reinterpret_cast<JitClosure*>(v.data);
    try {
      auto r = _culebra_invoke0(c);
      _culebra_value_release_impl(r.tag, r.data);
    } catch (...) {
      _culebra_value_release_impl(v.tag, v.data);
      // Drop remaining defers for this scope so they don't leak.
      while (static_cast<int64_t>(s.size()) > mark) {
        auto rem = s.back();
        s.pop_back();
        _culebra_value_release_impl(rem.tag, rem.data);
      }
      throw;
    }
    _culebra_value_release_impl(v.tag, v.data);
  }
}

inline const char* _culebra_tag_name(int8_t tag) {
  switch (tag) {
    case TAG_NIL:    return "Nil";
    case TAG_BOOL:   return "Bool";
    case TAG_LONG:   return "Long";
    case TAG_FUNC:   return "Function";
    case TAG_STRING: return "String";
    case TAG_ARRAY:  return "Array";
    case TAG_OBJECT: return "Object";
    case TAG_FLOAT:  return "Float";
    case TAG_TENSOR: return "Tensor";
    case TAG_TUPLE:  return "Tuple";
    case TAG_SET:    return "Set";
    case TAG_STRINGVIEW: return "StringView";
  }
  return "Unknown";
}

// Check that a Value matches a named type ("Any" matches everything).
// For class-instance arguments (TAG_OBJECT with a `class:` String
// property), also accept the matching class name — without this the
// JIT can't validate `s: Square` annotations against `Square.new(...)`.
// Test whether the runtime value at (tag, data) matches a single
// non-Union type name. Mirrors interp's type_matches for the
// primitive + Object/class side; called from culebra_runtime_type_check
// once per Union alternative.
inline bool _culebra_type_matches_single(int8_t tag, int64_t data,
                                          std::string_view expected) {
  if (expected == "Any") return true;
  // Generic outer-match: `Array<Long>` checks `Array` only. Element
  // type is documentation in the MVP (matches interp's type_matches).
  if (expected.find('<') != std::string_view::npos) {
    expected = culebra::parse_generic_head(expected).outer;
  }
  std::string_view actual = _culebra_tag_name(tag);
  if (actual == expected) return true;
  // Built-in trait conformance: primitives (non-Object tags) can
  // satisfy Stringer / Eq / Comparable via the hard-coded table.
  if (tag != TAG_OBJECT && culebra::lookup_trait(expected) &&
      culebra::builtin_conforms_to_trait(actual, expected)) {
    return true;
  }
  std::string_view class_tag_view;
  if (tag == TAG_OBJECT) {
    auto* obj = reinterpret_cast<JitObject*>(data);
    if (auto idx = obj->find_slot("class");
        idx != static_cast<size_t>(-1)) {
      const auto& cls_slot = obj->slots[idx].value;
      if (cls_slot.tag == TAG_STRING) {
        class_tag_view = std::string_view(
            reinterpret_cast<const char*>(cls_slot.data));
        if (class_tag_view == expected) return true;
      }
    }
  }
  // Structural trait conformance: when `expected` is a registered
  // trait, check (and cache) whether this instance's class supplies
  // every required method (matching arity). Primitive tags can't
  // conform (no class tag, no method table).
  if (auto* trait = culebra::lookup_trait(expected)) {
    if (tag != TAG_OBJECT) return false;
    if (class_tag_view.empty()) {
      // Bare Object literal — ObjectValue::builtins() provides defaults
      // (e.g. `iter()` for Iterable) so the built-in table answers.
      return culebra::builtin_conforms_to_trait("Object", expected);
    }
    std::string class_name(class_tag_view);
    std::string trait_name(expected);
    std::unique_lock lock(culebra::trait_mutex());
    auto& by_trait = culebra::trait_conformance_cache()[class_name];
    auto it = by_trait.find(trait_name);
    if (it != by_trait.end()) return it->second;
    auto* obj = reinterpret_cast<JitObject*>(data);
    std::unordered_map<std::string, size_t> class_methods;
    auto walk_slots = [&](JitObject* o) {
      if (!o || !o->shape) return;
      for (size_t i = 0; i < o->shape->names.size(); i++) {
        const auto& slot = o->slots[i];
        if (slot.value.tag != TAG_FUNC) continue;
        auto* cls = reinterpret_cast<JitClosure*>(slot.value.data);
        // JitClosure::arity counts user-visible params (excluding __cls__
        // and `this`), matching the interp side's positional count.
        class_methods.emplace(o->shape->names[i], cls->arity);
      }
    };
    // Methods live on the class meta (proto), data fields on the
    // instance itself. Walk both so e.g. `to_s` defined on the class
    // is visible to the conformance check.
    walk_slots(obj);
    walk_slots(obj->proto);
    bool conforms = culebra::class_conforms_to_trait(class_methods, *trait);
    by_trait[trait_name] = conforms;
    return conforms;
  }
  return false;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_type_matches(
    int8_t tag, int64_t data, const char* expected) {
  return _culebra_type_matches_single(tag, data, std::string_view(expected));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_type_check(
    int8_t tag, int64_t data, const char* expected, const char* context,
    int64_t line, int64_t col) {
  if (expected == nullptr || expected[0] == '\0') return;
  // `Any` annotation accepts every value — bail before the Union split
  // and per-alt match. Hot when generic helpers carry `: Any` params.
  if (std::strcmp(expected, "Any") == 0) return;
  // Union types: any-of match across pipe-separated alternatives.
  // Compare on string_view — no per-alternative std::string copy.
  // Depth-aware gate so `Array<Long | Float>`'s inner `|` doesn't
  // trigger a Union split that's then never used.
  std::string_view sv(expected);
  if (culebra::has_toplevel_pipe(sv)) {
    for (auto cand : culebra::split_union_types(sv)) {
      if (_culebra_type_matches_single(tag, data, cand)) return;
    }
    throw culebra::CulebraError("TypeError", std::format(
        "type error: {} expects {} at {}:{}.", context, expected, line, col),
        line, col);
  }
  if (_culebra_type_matches_single(tag, data, sv)) return;
  throw culebra::CulebraError("TypeError", std::format(
      "type error: {} expects {} at {}:{}.", context, expected, line, col),
      line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_div_zero(int64_t line,
                                                           int64_t col) {
  throw culebra::CulebraError("ZeroDivisionError",
      std::format("divide by 0 error at {}:{}.", line, col), line, col);
}

// --- Numeric runtime helpers (Float-aware arithmetic slow paths) ---
//
// Mirror eval_bin_op_step / eval_power in interpreter.h. The JIT
// emits an inline "both Long" fast path and only calls these when at
// least one operand is Float or the types are mixed.

// Invoke a method closure with an explicit `this`. Retains both the
// receiver and the argument so the callee can consume them per the
// JIT's closure ABI. Returns the +1 result.
inline JitValue _culebra_invoke_method1(JitClosure* cls, JitValue self,
                                        JitValue arg) {
  culebra_runtime_value_retain(self.tag, self.data);
  culebra_runtime_value_retain(arg.tag, arg.data);
  JitValue args[1] = {arg};
  return reinterpret_cast<JitFn>(cls->fn_ptr)(cls, self, 1, args);
}

inline JitValue _culebra_invoke_method0(JitClosure* cls, JitValue self) {
  culebra_runtime_value_retain(self.tag, self.data);
  return reinterpret_cast<JitFn>(cls->fn_ptr)(cls, self, 0, nullptr);
}

// Resolve user-defined `hash()` on an Object (Hashable structural
// conformance). Returns the Long payload on success; nullopt when the
// method is absent or returns a non-Long value (caller falls back to
// reference identity).
inline std::optional<int64_t> _jit_object_user_hash(JitObject* obj) {
  auto* entry = _find_property(obj, "hash");
  if (!entry || entry->value.tag != TAG_FUNC) return std::nullopt;
  auto* cls = reinterpret_cast<JitClosure*>(entry->value.data);
  auto r = _culebra_invoke_method0(
      cls, {TAG_OBJECT, reinterpret_cast<int64_t>(obj)});
  if (r.tag != TAG_LONG) {
    _culebra_value_release_impl(r.tag, r.data);
    return std::nullopt;
  }
  return r.data;
}

// Resolve user-defined `eq(other)` on a pair of Objects (Eq structural
// conformance). Both sides must expose `eq`; otherwise nullopt so the
// caller keeps reference equality.
inline std::optional<bool> _jit_object_user_eq(JitObject* a, JitObject* b) {
  auto* ea = _find_property(a, "eq");
  auto* eb = _find_property(b, "eq");
  if (!ea || !eb || ea->value.tag != TAG_FUNC || eb->value.tag != TAG_FUNC) {
    return std::nullopt;
  }
  auto* cls = reinterpret_cast<JitClosure*>(ea->value.data);
  auto r = _culebra_invoke_method1(
      cls, {TAG_OBJECT, reinterpret_cast<int64_t>(a)},
      {TAG_OBJECT, reinterpret_cast<int64_t>(b)});
  bool result = (r.tag == TAG_BOOL && r.data != 0);
  _culebra_value_release_impl(r.tag, r.data);
  return result;
}

// Look up a Function-typed property on a JitObject by name.
// Walk own props then (optionally) the proto chain (one level) for a
// matching property. Returns nullptr if absent. Shared helper for
// every property reader on the JIT side; keeps the proto-fallthrough
// rule in a single place.
inline const JitObjectEntry* _find_property(JitObject* obj,
                                             const char* key) {
  auto idx = obj->find_slot(key);
  if (idx != static_cast<size_t>(-1)) return &obj->slots[idx];
  if (obj->proto) {
    idx = obj->proto->find_slot(key);
    if (idx != static_cast<size_t>(-1)) return &obj->proto->slots[idx];
  }
  return nullptr;
}

inline JitClosure* _lookup_special(int8_t tag, int64_t data, const char* name) {
  if (tag != TAG_OBJECT) return nullptr;
  auto* obj = reinterpret_cast<JitObject*>(data);
  auto* entry = _find_property(obj, name);
  if (!entry || entry->value.tag != TAG_FUNC) return nullptr;
  return reinterpret_cast<JitClosure*>(entry->value.data);
}

// Invoke a special method `recv.<name>(arg)`. Returns the +1 result
// or std::nullopt.
inline std::optional<JitValue> _try_special_binop(int8_t rt, int64_t rd,
                                                 int8_t at, int64_t ad,
                                                 const char* name) {
  auto* cls = _lookup_special(rt, rd, name);
  if (!cls) return std::nullopt;
  return _culebra_invoke_method1(cls, {rt, rd}, {at, ad});
}

inline std::optional<JitValue> _try_special_unary(int8_t t, int64_t d,
                                                 const char* name) {
  auto* cls = _lookup_special(t, d, name);
  if (!cls) return std::nullopt;
  return _culebra_invoke_method0(cls, {t, d});
}

// Invoke `__str__` on an Object, copying the returned String into an
// owned std::string and releasing the method's +1 return. Returns
// nullopt for non-Objects or Objects without `__str__`; throws on
// non-String returns so a buggy method fails loudly.
inline std::optional<std::string> _try_str_special(int8_t type, int64_t data) {
  auto r = _try_special_unary(type, data, "__str__");
  if (!r) return std::nullopt;
  if (r->tag != TAG_STRING && r->tag != TAG_STRINGVIEW) {
    _culebra_value_release_impl(r->tag, r->data);
    throw culebra::CulebraError("TypeError",
                                "__str__ must return a String");
  }
  std::string out(_culebra_str_view(r->tag, r->data));
  _culebra_value_release_impl(r->tag, r->data);
  return out;
}

// Arithmetic binop: try `lhs.__op__(rhs)`; if `reflect` is true and
// nothing matched, try `rhs.__op__(lhs)` (commutative auto-reflection
// for `+` and `*`). Callers fall back to the numeric path otherwise.
inline std::optional<JitValue> _dispatch_arith_special(int8_t lt, int64_t ld,
                                                     int8_t rt, int64_t rd,
                                                     const char* name,
                                                     bool reflect) {
  if (auto r = _try_special_binop(lt, ld, rt, rd, name)) return r;
  if (reflect) {
    if (auto r = _try_special_binop(rt, rd, lt, ld, name)) return r;
  }
  return std::nullopt;
}

// Forward decl — body is further down with the other Tensor runtime entries.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_binop(
    int8_t lt, int64_t ld, int8_t rt_, int64_t rd, int64_t op_id);

// Routes binop through the Tensor runtime when either operand is a
// Tensor. M2.0 requires both operands to be Tensor; scalar broadcast
// lands in M2.1.
inline std::optional<JitValue> _try_tensor_binop(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd, int op_id) {
#ifdef CULEBRA_RT_NO_TENSOR
  (void)lt; (void)ld; (void)rt; (void)rd; (void)op_id;
  return std::nullopt;
#else
  if (lt != TAG_TENSOR && rt != TAG_TENSOR) return std::nullopt;
  auto* t = culebra_runtime_tensor_binop(lt, ld, rt, rd, op_id);
  return JitValue{TAG_TENSOR, reinterpret_cast<int64_t>(t)};
#endif
}

#define CUL_NUM_BINOP(name, method, expr, reflect, op_id)               \
  CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_num_##name(     \
      int8_t lt, int64_t ld, int8_t rt, int64_t rd) {                   \
    if (auto r = _try_tensor_binop(lt, ld, rt, rd, op_id)) return *r;   \
    if (auto r = _dispatch_arith_special(lt, ld, rt, rd, method,        \
                                         reflect))                      \
      return *r;                                                        \
    auto a = _culebra_coerce_num(lt, ld);                               \
    auto b = _culebra_coerce_num(rt, rd);                               \
    return {TAG_FLOAT, _culebra_double_to_bits(expr)};                  \
  }
CUL_NUM_BINOP(add, "__add__", a + b, true,  static_cast<int>(culebra::Op::Add))
CUL_NUM_BINOP(sub, "__sub__", a - b, false, static_cast<int>(culebra::Op::Sub))
CUL_NUM_BINOP(mul, "__mul__", a * b, true,  static_cast<int>(culebra::Op::Mul))
#undef CUL_NUM_BINOP

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_num_div(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd) {
  if (auto r = _try_tensor_binop(lt, ld, rt, rd,
                                  static_cast<int>(culebra::Op::Div)))
    return *r;
  if (auto r = _dispatch_arith_special(lt, ld, rt, rd, "__div__", false))
    return *r;
  auto a = _culebra_coerce_num(lt, ld);
  auto b = _culebra_coerce_num(rt, rd);
  if (b == 0.0) throw culebra::CulebraError("ZeroDivisionError", "divide by 0 error");
  return {TAG_FLOAT, _culebra_double_to_bits(a / b)};
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_num_mod(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd) {
  if (auto r = _dispatch_arith_special(lt, ld, rt, rd, "__mod__", false))
    return *r;
  auto a = _culebra_coerce_num(lt, ld);
  auto b = _culebra_coerce_num(rt, rd);
  if (b == 0.0) throw culebra::CulebraError("ZeroDivisionError", "divide by 0 error");
  return {TAG_FLOAT, _culebra_double_to_bits(std::fmod(a, b))};
}

// `@` (matmul) has no numeric meaning — always dispatches through
// `__matmul__`. Non-commutative, so no reflection.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_num_matmul(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd) {
  if (auto r = _try_special_binop(lt, ld, rt, rd, "__matmul__")) return *r;
  throw culebra::CulebraError("TypeError", "type error.");
}

// Extract a boolean from a special method's +1 return value and release
// the (potentially heap-backed) result. Matches `Value::to_bool`:
// accepts Bool/Long/Float and throws `type error` on anything else, so
// a comparison method that forgets to return a boolean fails loudly.
inline bool _extract_bool_and_release(JitValue v) {
  bool b;
  if (v.tag == TAG_BOOL) b = v.data != 0;
  else if (v.tag == TAG_LONG) b = v.data != 0;
  else if (v.tag == TAG_FLOAT) b = _culebra_float_to_double(v.data) != 0.0;
  else {
    // Match interp's `Value::to_bool()` message so cross-backend
    // try/catch text comparisons stay aligned.
    auto got = std::string(_culebra_tag_name(v.tag));
    _culebra_value_release_impl(v.tag, v.data);
    throw culebra::CulebraError("TypeError",
        std::format("type error: expected Bool, Long, or Float, got {}",
                    got));
  }
  _culebra_value_release_impl(v.tag, v.data);
  return b;
}

// extern "C" entry points for the comparison helpers. One per
// operator so the JIT can look them up by name.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_value_equal(
    int8_t t1, int64_t d1, int8_t t2, int64_t d2) {
  // `==` is commutative, so try either side's `__eq__`.
  if (auto r = _try_special_binop(t1, d1, t2, d2, "__eq__"))
    return _extract_bool_and_release(*r);
  if (auto r = _try_special_binop(t2, d2, t1, d1, "__eq__"))
    return _extract_bool_and_release(*r);
  return _culebra_value_equal(t1, d1, t2, d2);
}

// Try `lhs.__le__(rhs)`, falling back to `__lt__` || `__eq__` to match
// the interpreter's derivation when a class only defines `__lt__`.
inline std::optional<bool> _special_le(int8_t t1, int64_t d1,
                                      int8_t t2, int64_t d2) {
  if (auto r = _try_special_binop(t1, d1, t2, d2, "__le__"))
    return _extract_bool_and_release(*r);
  auto lt = _try_special_binop(t1, d1, t2, d2, "__lt__");
  auto eq = _try_special_binop(t1, d1, t2, d2, "__eq__");
  if (!lt && !eq) return std::nullopt;
  bool l = lt && _extract_bool_and_release(*lt);
  bool e = eq && _extract_bool_and_release(*eq);
  return l || e;
}

#define CUL_DEF_ORD_OP(name, cmp_op, fast_path)                         \
  CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_value_##name(       \
      int8_t t1, int64_t d1, int8_t t2, int64_t d2) {                   \
    fast_path                                                           \
    return _culebra_value_ord(t1, d1, t2, d2,                           \
                              [](double a, double b) { return a cmp_op b; }); \
  }
CUL_DEF_ORD_OP(less, <,
  if (auto r = _try_special_binop(t1, d1, t2, d2, "__lt__"))
    return _extract_bool_and_release(*r);
)
CUL_DEF_ORD_OP(leq, <=,
  if (auto r = _special_le(t1, d1, t2, d2)) return *r;
)
// a > b ≡ !(a <= b)
CUL_DEF_ORD_OP(greater, >,
  if (auto r = _special_le(t1, d1, t2, d2)) return !*r;
)
// a >= b ≡ !(a < b)
CUL_DEF_ORD_OP(geq, >=,
  if (auto r = _try_special_binop(t1, d1, t2, d2, "__lt__"))
    return !_extract_bool_and_release(*r);
)
#undef CUL_DEF_ORD_OP

// Power with full Python-style semantics:
//   Long ** non-negative Long  → Long (exp-by-squaring, wraps)
//   Long ** negative Long      → Float (promotes via std::pow)
//   any Float                  → Float
// In-place Tensor compound assignment: `lhs OP= rhs`. When lhs is a
// Tensor that owns its storage and the op is + - * / **, mutate lhs's
// buffer directly (saves the per-step allocation in SGD-style loops).
// Falls back to the regular binop helper for non-Tensor lhs or when
// the in-place precondition fails (so the caller's ABI is identical
// to the regular num_OP helper).
inline JitValue _try_tensor_inplace(int8_t lt, int64_t ld,
                                    int8_t rt, int64_t rd,
                                    culebra::Op op) {
#ifdef CULEBRA_RT_NO_TENSOR
  (void)lt; (void)ld; (void)rt; (void)rd; (void)op;
  return {TAG_NIL, 0};
#else
  auto* lhs_t = reinterpret_cast<JitTensor*>(ld);
  culebra::TensorPtr rhs;
  if (rt == TAG_TENSOR) {
    rhs = reinterpret_cast<JitTensor*>(rd)->impl;
  } else {
    rhs = culebra::tensor_scalar(_culebra_coerce_num(rt, rd),
                                 lhs_t->impl->dtype);
  }
  if (culebra::tensor_inplace_binop(*lhs_t->impl, op, std::move(rhs))) {
    culebra_runtime_value_retain(lt, ld);
    return {lt, ld};
  }
  return {TAG_NIL, 0};  // sentinel: in-place did not run
#endif
}

#define CUL_NUM_INPLACE(name, op_enum)                                  \
  CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue                                 \
  culebra_runtime_num_inplace_##name(                                   \
      int8_t lt, int64_t ld, int8_t rt, int64_t rd) {                   \
    if (lt == TAG_TENSOR) {                                             \
      auto r = _try_tensor_inplace(lt, ld, rt, rd, culebra::op_enum);   \
      if (r.tag != TAG_NIL) return r;                                   \
    }                                                                   \
    return culebra_runtime_num_##name(lt, ld, rt, rd);                  \
  }
CUL_NUM_INPLACE(add, Op::Add)
CUL_NUM_INPLACE(sub, Op::Sub)
CUL_NUM_INPLACE(mul, Op::Mul)
CUL_NUM_INPLACE(div, Op::Div)
// Pow expansion is further down — it needs `culebra_runtime_num_pow`.

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_num_pow(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd) {
  if (auto r = _try_special_binop(lt, ld, rt, rd, "__pow__")) return *r;
  if (lt == TAG_LONG && rt == TAG_LONG) {
    int64_t a = ld, e = rd;
    if (e >= 0) return {TAG_LONG, culebra::ipow_nonneg(a, e)};
    if (a == 0) throw culebra::CulebraError("ZeroDivisionError", "divide by 0 error");
    return {TAG_FLOAT,
            _culebra_double_to_bits(std::pow(static_cast<double>(a),
                                             static_cast<double>(e)))};
  }
  auto a = _culebra_coerce_num(lt, ld);
  auto b = _culebra_coerce_num(rt, rd);
  return {TAG_FLOAT, _culebra_double_to_bits(std::pow(a, b))};
}

CUL_NUM_INPLACE(pow, Op::Pow)
#undef CUL_NUM_INPLACE

// Unary negation: Long → Long (wraps), Float → Float. Non-numeric
// raises type error. Called only from the unary-minus slow path.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_num_neg(
    int8_t t, int64_t d) {
  if (auto r = _try_special_unary(t, d, "__neg__")) return *r;
  if (t == TAG_LONG) return {TAG_LONG, -d};
  if (t == TAG_FLOAT) {
    auto v = _culebra_float_to_double(d);
    return {TAG_FLOAT, _culebra_double_to_bits(-v)};
  }
  throw culebra::CulebraError("TypeError", "type error.");
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_debugger_break(
    const char* path, int64_t line, int64_t col) {
  std::println(stderr, "\nBreak in {}:{}:{}", path, line, col);
  // Show a few lines of source around the break point, if we can open it
  std::ifstream ifs(path);
  if (ifs) {
    std::string src((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
    std::vector<std::string> lines;
    std::string cur;
    for (auto c : src) {
      if (c == '\n') { lines.push_back(cur); cur.clear(); }
      else cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);

    auto start = line > 2 ? static_cast<size_t>(line) - 3 : 0;
    auto end = std::min(lines.size(),
                        static_cast<size_t>(line) + 2);
    for (auto i = start; i < end; i++) {
      auto marker = (i + 1 == static_cast<size_t>(line)) ? ">" : " ";
      std::println(stderr, "{} {:>4} {}", marker, i + 1, lines[i]);
    }
  }
  std::print(stderr, "\ndebug> ");
  std::string s;
  std::getline(std::cin, s);
  // Any input continues execution (minimal: no commands yet)
}

// --- Array runtime ---

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_new() {
  auto* arr = new JitArray();
  arr->refcount = 1;
  arr->size = 0;
  arr->capacity = 0;
  arr->items = nullptr;
  _gc().add(arr, GC_TAG_ARRAY);
  return arr;
}

// Allocate an Array with `capacity` slots already reserved. Used by
// inlined HOF loops (see emit_inlined_array_map) so per-iteration
// `array_push` doesn't re-grow the buffer log(N) times. `size`
// stays 0 — push fills it as elements arrive.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_new_reserved(
    int64_t capacity) {
  auto* arr = new JitArray();
  arr->refcount = 1;
  arr->size = 0;
  arr->capacity = capacity > 0 ? static_cast<size_t>(capacity) : 0;
  arr->items = capacity > 0 ? new JitValue[arr->capacity] : nullptr;
  _gc().add(arr, GC_TAG_ARRAY);
  return arr;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_set_or_push(
    JitArray* arr, int64_t idx, int8_t tag, int64_t data);

// Tuple sits on the same JitArray storage but is tracked under
// GC_TAG_TUPLE so the cycle collector dispatches via the tuple-aware
// `_gc_slot_of` path. Caller-visible immutability is enforced by the
// IR (mutation ops only accept TAG_ARRAY) — there is no mutating
// runtime entry for Tuple.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_tuple_new() {
  auto* arr = new JitArray();
  arr->refcount = 1;
  arr->size = 0;
  arr->capacity = 0;
  arr->items = nullptr;
  _gc().add(arr, GC_TAG_TUPLE);
  return arr;
}

// Build-time element append. Reuses array_push internals (forward
// declared and defined below) because the layout is identical; only
// the GC tag differs.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_push(JitArray* arr,
                                                             int8_t tag,
                                                             int64_t data);
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_tuple_push(JitArray* arr,
                                                             int8_t tag,
                                                             int64_t data) {
  culebra_runtime_array_push(arr, tag, data);
}

// Set runtime: insertion-ordered with O(1) membership.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitSet* culebra_runtime_set_new() {
  auto* s = new JitSet();
  s->refcount = 1;
  s->index = new JitSetIndex();
  _gc().add(s, GC_TAG_SET);
  return s;
}

// Add `value` to `set` unless already present. The set absorbs the
// +1 reference on hit; on a duplicate we release it so the caller's
// ownership transfer balances out.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_set_add(JitSet* set,
                                                          int8_t tag,
                                                          int64_t data) {
  JitValue v{tag, data};
  if (!set->index->insert(v).second) {
    _culebra_value_release_impl(tag, data);
    return;
  }
  set->members.push_back(v);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_set_contains(JitSet* set,
                                                               int8_t tag,
                                                               int64_t data) {
  return set->index && set->index->contains(JitValue{tag, data});
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_set_size(JitSet* set) {
  return static_cast<int64_t>(set->members.size());
}

// Returns a fresh +1 Set whose members are the union / intersection /
// (a - b) of `a` and `b`. Each member is retained once into the result.
// Common tail for set_union/intersect/diff: take +1 ownership of `v`
// into `out` if not already present. Source members are unique within
// their own set, so the dedup check is only meaningful for `union`.
inline void _set_take(JitSet* out, const JitValue& v) {
  if (!out->index->insert(v).second) return;
  culebra_runtime_value_retain(v.tag, v.data);
  out->members.push_back(v);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitSet* culebra_runtime_set_union(JitSet* a,
                                                               JitSet* b) {
  auto* out = culebra_runtime_set_new();
  for (auto& v : a->members) _set_take(out, v);
  for (auto& v : b->members) _set_take(out, v);
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitSet* culebra_runtime_set_intersect(JitSet* a,
                                                                   JitSet* b) {
  auto* out = culebra_runtime_set_new();
  for (auto& v : a->members) {
    if (b->index->contains(v)) _set_take(out, v);
  }
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitSet* culebra_runtime_set_diff(JitSet* a,
                                                              JitSet* b) {
  auto* out = culebra_runtime_set_new();
  for (auto& v : a->members) {
    if (!b->index->contains(v)) _set_take(out, v);
  }
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitSet* culebra_runtime_set_sym_diff(JitSet* a,
                                                                  JitSet* b) {
  auto* out = culebra_runtime_set_new();
  for (auto& v : a->members) {
    if (!b->index->contains(v)) _set_take(out, v);
  }
  for (auto& v : b->members) {
    if (!a->index->contains(v)) _set_take(out, v);
  }
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_set_subset(JitSet* a,
                                                               JitSet* b) {
  for (auto& v : a->members) {
    if (!b->index->contains(v)) return 0;
  }
  return 1;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_set_superset(JitSet* a,
                                                                 JitSet* b) {
  for (auto& v : b->members) {
    if (!a->index->contains(v)) return 0;
  }
  return 1;
}

// Mutating add: returns 1 on insert, 0 if already present. Hands the
// caller's +1 reference into the set on insert; releases it on dup.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_set_add_method(
    JitSet* set, int8_t tag, int64_t data) {
  JitValue v{tag, data};
  if (!set->index->insert(v).second) {
    _culebra_value_release_impl(tag, data);
    return 0;
  }
  set->members.push_back(v);
  return 1;
}

// Mutating remove: returns 1 if removed, 0 if absent.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_set_remove(JitSet* set,
                                                               int8_t tag,
                                                               int64_t data) {
  JitValue key{tag, data};
  if (!set->index->erase(key)) return 0;
  // Find and erase from the members vector (O(n) — same as the interp's
  // OrderedSymbolMap erase pattern).
  for (auto it = set->members.begin(); it != set->members.end(); ++it) {
    if (JitValueEq{}(*it, key)) {
      _culebra_value_release_impl(it->tag, it->data);
      set->members.erase(it);
      return 1;
    }
  }
  return 0;  // unreachable if index and members stayed in sync
}

// Materialize a Set as a fresh Array, retaining each element.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_set_to_array(
    JitSet* set) {
  auto* arr = culebra_runtime_array_new();
  for (auto& v : set->members) {
    culebra_runtime_value_retain(v.tag, v.data);
    culebra_runtime_array_push(arr, v.tag, v.data);
  }
  return arr;
}

// Materialize a Tuple as a fresh Array, retaining each element.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_tuple_to_array(
    JitArray* tup) {
  auto* arr = culebra_runtime_array_new();
  for (size_t i = 0; i < tup->size; i++) {
    auto& e = tup->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    culebra_runtime_array_push(arr, e.tag, e.data);
  }
  return arr;
}

// Tuple element search (linear). Returns 1 if `v` is present, 0 otherwise.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_tuple_contains(
    JitArray* tup, int8_t tag, int64_t data) {
  for (size_t i = 0; i < tup->size; i++) {
    if (_culebra_value_equal(tup->items[i].tag, tup->items[i].data,
                              tag, data)) return 1;
  }
  return 0;
}

}  // extern "C"

// Forward declaration for runtime helpers that need refcount logic
inline void _culebra_value_release_impl(int8_t tag, int64_t data);

extern "C" {

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_push(JitArray* arr,
                                                             int8_t tag,
                                                             int64_t data) {
  if (arr->size >= arr->capacity) {
    size_t new_cap = arr->capacity * 2;
    if (new_cap < 8) new_cap = 8;
    auto* new_items = new JitValue[new_cap];
    if (arr->items) {
      std::memcpy(new_items, arr->items, arr->size * sizeof(JitValue));
      delete[] arr->items;
    }
    arr->items = new_items;
    arr->capacity = new_cap;
  }
  // Array absorbs ownership of the pushed value (+1).
  arr->items[arr->size].tag = tag;
  arr->items[arr->size].data = data;
  arr->size++;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_resize(
    JitArray* arr, int64_t count, int8_t def_tag, int64_t def_data) {
  while (arr->size < static_cast<size_t>(count)) {
    culebra_runtime_array_push(arr, def_tag, def_data);
  }
  if (static_cast<size_t>(count) < arr->size) {
    arr->size = static_cast<size_t>(count);
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_get(JitArray* arr,
                                                            int64_t idx,
                                                            int8_t* out_tag,
                                                            int64_t* out_data,
                                                            int64_t line,
                                                            int64_t col) {
  if (idx < 0) idx = static_cast<int64_t>(arr->size) + idx;
  if (idx < 0 || static_cast<size_t>(idx) >= arr->size) {
    throw culebra::CulebraError("IndexError",
        std::format("index out of range at {}:{}.", line, col), line, col);
  }
  *out_tag = arr->items[idx].tag;
  *out_data = arr->items[idx].data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_set(JitArray* arr,
                                                            int64_t idx,
                                                            int8_t tag,
                                                            int64_t data,
                                                            int64_t line,
                                                            int64_t col) {
  if (idx < 0 || static_cast<size_t>(idx) >= arr->size) {
    throw culebra::CulebraError("IndexError",
        std::format("index out of range at {}:{}.", line, col), line, col);
  }
  _culebra_value_release_impl(arr->items[idx].tag, arr->items[idx].data);
  arr->items[idx].tag = tag;
  arr->items[idx].data = data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_array_size(JitArray* arr) {
  return static_cast<int64_t>(arr->size);
}

// Forward decl (defined later alongside the refcount runtime).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_value_retain(int8_t tag,
                                                               int64_t data);

// Create a new array by copying [start, start+len) from src. Each copied
// element is retained (new array holds another reference).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_slice(
    JitArray* src, int64_t start, int64_t len) {
  auto* r = culebra_runtime_array_new();
  for (int64_t i = 0; i < len; i++) {
    auto& e = src->items[start + i];
    culebra_runtime_value_retain(e.tag, e.data);
    culebra_runtime_array_push(r, e.tag, e.data);
  }
  return r;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_set_or_push(
    JitArray* arr, int64_t idx, int8_t tag, int64_t data) {
  if (static_cast<size_t>(idx) < arr->size) {
    _culebra_value_release_impl(arr->items[idx].tag, arr->items[idx].data);
    arr->items[idx].tag = tag;
    arr->items[idx].data = data;
  } else {
    culebra_runtime_array_push(arr, tag, data);
  }
}

// --- Tensor runtime ---

// args layout: [optional "f32"/"f64" string, shape varargs OR single
// Array of Long]. Mirrors the interpreter's parse_tensor_dtype_prefix
// + parse_tensor_shape pair.
inline std::pair<culebra::Dtype, culebra::TensorShape>
_culebra_parse_tensor_ctor_args(const JitValue* args, int64_t n, int64_t line,
                                int64_t col) {
  auto type_err = [&]() {
    culebra::throw_type_error_at(line, col);
  };
  culebra::Dtype dt = culebra::Dtype::F32;
  int64_t off = 0;
  if (n > 0 && args[0].tag == TAG_STRING) {
    auto d = culebra::parse_dtype(reinterpret_cast<const char*>(args[0].data));
    if (!d) type_err();
    dt = *d;
    off = 1;
  }
  std::vector<int64_t> dims;
  auto push_long = [&](const JitValue& v) {
    if (v.tag != TAG_LONG) type_err();
    dims.push_back(v.data);
  };
  if (n - off == 1 && args[off].tag == TAG_ARRAY) {
    auto* a = reinterpret_cast<JitArray*>(args[off].data);
    for (size_t i = 0; i < a->size; i++) push_long(a->items[i]);
  } else {
    for (int64_t i = off; i < n; i++) push_long(args[i]);
  }
  return {dt, culebra::TensorShape(std::move(dims))};
}

#ifdef CULEBRA_RT_NO_TENSOR

// Stub block: polymorphic dispatch (e.g. `Array.slice` / `Array.shape`
// in compile_call) statically references the TAG_TENSOR branch even
// when AST scan reports no Tensor use. These stubs satisfy the linker
// without dragging in tensor.h / cblas; at runtime no tensor value
// can be created in a NO_TENSOR binary so the branches stay
// unreachable. Bodies throw defensively in case that invariant ever
// breaks — the AOT bootstrap catches CulebraError and produces the
// same diagnostic format as `JIT::exec`.

[[noreturn]] inline void _no_tensor_abort() {
  throw culebra::CulebraError(
      "InternalError",
      "tensor runtime entered in NO_TENSOR binary", 0, 0);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_zeros(
    const JitValue*, int64_t, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_ones(
    const JitValue*, int64_t, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_randn(
    const JitValue*, int64_t, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_from(
    JitArray*, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_tensor_shape(
    JitTensor*) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_binop(
    int8_t, int64_t, int8_t, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_tensor_eval_one(
    JitTensor*) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_transpose(
    JitTensor*) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_clone(
    JitTensor*) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_slice(
    JitTensor*, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_tensor_to_array(
    JitTensor*, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_from_csv(
    const char*, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_dot(
    JitTensor*, JitTensor*, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_unary(
    JitTensor*, int64_t, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_linear_sigmoid(
    JitTensor*, JitTensor*, JitTensor*, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_reduce_axis(
    JitTensor*, int64_t, int64_t, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_tensor_reduce_all(
    JitTensor*, int64_t, int64_t, int64_t) { _no_tensor_abort(); }
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_reshape(
    JitTensor*, JitArray*) { _no_tensor_abort(); }

#else  // !CULEBRA_RT_NO_TENSOR

inline JitTensor* _culebra_jit_tensor_register(culebra::TensorPtr impl) {
  auto* t = new JitTensor{1, -1, std::move(impl)};
  _gc().add(t, GC_TAG_TENSOR);
  return t;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_zeros(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  auto [dt, shape] = _culebra_parse_tensor_ctor_args(args, n, line, col);
  return _culebra_jit_tensor_register(culebra::tensor_zeros(std::move(shape), dt));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_ones(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  auto [dt, shape] = _culebra_parse_tensor_ctor_args(args, n, line, col);
  return _culebra_jit_tensor_register(culebra::tensor_ones(std::move(shape), dt));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_randn(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  auto [dt, shape] = _culebra_parse_tensor_ctor_args(args, n, line, col);
  return _culebra_jit_tensor_register(culebra::tensor_randn(std::move(shape), dt));
}

// Tensor.from(arr): walk a 1D or 2D nested JitArray. M1 only F32; an
// optional dtype tag arrives in M2.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_from(
    JitArray* a, int64_t line, int64_t col) {
  auto type_err = [&]() {
    culebra::throw_type_error_at(line, col);
  };
  auto coerce = [&](const JitValue& v) {
    if (v.tag == TAG_LONG) return static_cast<double>(v.data);
    if (v.tag == TAG_FLOAT) return _culebra_float_to_double(v.data);
    type_err();
    return 0.0;
  };
  using culebra::TensorImpl;
  using culebra::TensorShape;
  using culebra::Dtype;
  if (a->size == 0) {
    return _culebra_jit_tensor_register(
        std::make_shared<TensorImpl>(TensorShape({0}), Dtype::F32));
  }
  if (a->items[0].tag == TAG_LONG || a->items[0].tag == TAG_FLOAT) {
    auto impl = std::make_shared<TensorImpl>(
        TensorShape({static_cast<int64_t>(a->size)}), Dtype::F32);
    auto* p = impl->data_as<float>();
    for (size_t i = 0; i < a->size; i++) {
      p[i] = static_cast<float>(coerce(a->items[i]));
    }
    return _culebra_jit_tensor_register(std::move(impl));
  }
  if (a->items[0].tag != TAG_ARRAY) type_err();
  auto* row0 = reinterpret_cast<JitArray*>(a->items[0].data);
  size_t cols = row0->size;
  size_t rows = a->size;
  auto impl = std::make_shared<TensorImpl>(
      TensorShape({static_cast<int64_t>(rows), static_cast<int64_t>(cols)}),
      Dtype::F32);
  auto* p = impl->data_as<float>();
  for (size_t i = 0; i < rows; i++) {
    if (a->items[i].tag != TAG_ARRAY) type_err();
    auto* row = reinterpret_cast<JitArray*>(a->items[i].data);
    if (row->size != cols) type_err();
    for (size_t j = 0; j < cols; j++) {
      p[i * cols + j] = static_cast<float>(coerce(row->items[j]));
    }
  }
  return _culebra_jit_tensor_register(std::move(impl));
}

// .shape() — returns a fresh JitArray of Long.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_tensor_shape(
    JitTensor* t) {
  auto* a = culebra_runtime_array_new();
  for (auto d : t->impl->shape.dims) {
    culebra_runtime_array_push(a, TAG_LONG, d);
  }
  return a;
}

// Build a lazy elementwise binop. At least one operand must be
// TAG_TENSOR; scalars (Long/Float) are lifted to a rank-0 Tensor with
// the other side's dtype, then broadcast handles the rest.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_binop(
    int8_t lt, int64_t ld, int8_t rt_, int64_t rd, int64_t op_id) {
  culebra::Dtype dt =
      reinterpret_cast<JitTensor*>(lt == TAG_TENSOR ? ld : rd)->impl->dtype;
  auto lift = [&](int8_t tag, int64_t data) -> culebra::TensorPtr {
    if (tag == TAG_TENSOR) return reinterpret_cast<JitTensor*>(data)->impl;
    return culebra::tensor_scalar(_culebra_coerce_num(tag, data), dt);
  };
  return _culebra_jit_tensor_register(culebra::tensor_binop(
      static_cast<culebra::Op>(op_id), lift(lt, ld), lift(rt_, rd)));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_tensor_eval_one(
    JitTensor* t) {
  culebra::tensor_eval_node(*t->impl);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_transpose(
    JitTensor* t) {
  return _culebra_jit_tensor_register(culebra::tensor_transpose(t->impl));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_clone(
    JitTensor* t) {
  return _culebra_jit_tensor_register(culebra::tensor_clone(t->impl));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_slice(
    JitTensor* t, int64_t start, int64_t end) {
  return _culebra_jit_tensor_register(
      culebra::tensor_slice(t->impl, start, end));
}

// Forces eval and returns a Culebra Array. Rank 1 → flat Array of
// Float; rank 2 → Array of Array of Float. Higher ranks are not
// supported in Phase 1.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_tensor_to_array(
    JitTensor* t) {
  culebra::tensor_eval_node(*t->impl);
  const auto& impl = *t->impl;
  auto read_at = [&](int64_t flat_idx) -> int64_t {
    double v = (impl.dtype == culebra::Dtype::F32)
        ? static_cast<double>(impl.data_as<float>()[flat_idx])
        : impl.data_as<double>()[flat_idx];
    return _culebra_double_to_bits(v);
  };
  const auto& dims = impl.shape.dims;
  if (dims.size() == 1) {
    auto* out = culebra_runtime_array_new();
    for (int64_t i = 0; i < dims[0]; i++) {
      culebra_runtime_array_push(out, TAG_FLOAT,
                                  read_at(i * impl.strides[0]));
    }
    return out;
  }
  if (dims.size() == 2) {
    auto* out = culebra_runtime_array_new();
    for (int64_t i = 0; i < dims[0]; i++) {
      auto* row = culebra_runtime_array_new();
      for (int64_t j = 0; j < dims[1]; j++) {
        culebra_runtime_array_push(
            row, TAG_FLOAT,
            read_at(i * impl.strides[0] + j * impl.strides[1]));
      }
      culebra_runtime_array_push(out, TAG_ARRAY,
                                  reinterpret_cast<int64_t>(row));
    }
    return out;
  }
  throw culebra::CulebraError("ValueError",
      "Tensor.to_array: rank > 2 not supported.");
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_from_csv(
    const char* path) {
  return _culebra_jit_tensor_register(
      culebra::tensor_from_csv(std::string(path), culebra::Dtype::F32));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_dot(
    JitTensor* a, JitTensor* b) {
  return _culebra_jit_tensor_register(culebra::tensor_dot(a->impl, b->impl));
}

// Unary activations (sigmoid / relu / softmax). op_id selects which.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_unary(
    JitTensor* a, int64_t op_id) {
  return _culebra_jit_tensor_register(
      culebra::tensor_unary(static_cast<culebra::Op>(op_id), a->impl));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_linear_sigmoid(
    JitTensor* W, JitTensor* x, JitTensor* b) {
  return _culebra_jit_tensor_register(
      culebra::tensor_linear_sigmoid(W->impl, x->impl, b->impl));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_reduce_axis(
    JitTensor* t, int64_t op_id, int64_t axis) {
  return _culebra_jit_tensor_register(culebra::tensor_reduce_axis(
      static_cast<culebra::Op>(op_id), t->impl, axis));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_tensor_reduce_all(
    JitTensor* t, int64_t op_id) {
  using culebra::Op;
  double v = 0.0;
  switch (static_cast<Op>(op_id)) {
    case Op::Sum:  v = culebra::tensor_reduce_all<Op::Sum>(t->impl); break;
    case Op::Mean: v = culebra::tensor_reduce_all<Op::Mean>(t->impl); break;
    case Op::Max:  v = culebra::tensor_reduce_all<Op::Max>(t->impl); break;
    default: throw std::runtime_error("tensor: invalid reduce op_id");
  }
  return {TAG_FLOAT, _culebra_double_to_bits(v)};
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_reshape(
    JitTensor* t, JitArray* dims) {
  std::vector<int64_t> new_dims;
  new_dims.reserve(dims->size);
  for (size_t i = 0; i < dims->size; i++) {
    if (dims->items[i].tag != TAG_LONG) {
      throw culebra::CulebraError("TypeError", "type error.");
    }
    new_dims.push_back(dims->items[i].data);
  }
  return _culebra_jit_tensor_register(culebra::tensor_reshape(
      t->impl, culebra::TensorShape(std::move(new_dims))));
}

#endif  // !CULEBRA_RT_NO_TENSOR

// --- Object runtime ---

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_object_new() {
  auto* o = new JitObject();
  o->refcount = 1;
  _gc().add(o, GC_TAG_OBJECT);
  return o;
}

// Build an Array from args[start..n) for binding to `__ARGS__`. Caller
// transferred +1 ownership of each arg via the stack-allocated slab;
// Array takes over (object_set-style: no extra retain, slot owns +1).
// Returns a fresh JitArray with refcount 1.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_args_slice_to_array(
    const JitValue* args, int64_t start, int64_t n) {
  auto* a = culebra_runtime_array_new();
  for (int64_t i = start; i < n; i++) {
    culebra_runtime_array_push(a, args[i].tag, args[i].data);
  }
  return a;
}

// When the function body never references `__ARGS__`, the prologue
// skips building the overflow Array — but the caller still transferred
// +1 retains on every overflow arg via the slab, so those refs must
// be released to balance the call. Cheap loop, called only when
// `n_args > declaredArity`.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_release_overflow_args(
    const JitValue* args, int64_t start, int64_t n) {
  for (int64_t i = start; i < n; i++) {
    _culebra_value_release_impl(args[i].tag, args[i].data);
  }
}

// Validate the well-known-property contract (see shared.h)
// for a freshly-bound JIT value: must be a 0-arg Function. The arg's
// +1 is released before throwing — codegen passes ownership and
// expects either store-into-map or release.
inline void _culebra_check_well_known_prop(std::string_view name,
                                           int8_t tag, int64_t data) {
  if (!culebra::is_well_known_prop(name)) return;
  auto bad = [&]() {
    _culebra_value_release_impl(tag, data);
    culebra::throw_well_known_prop_contract_error(name);
  };
  if (tag != GC_TAG_FUNC) bad();
  auto* cls = reinterpret_cast<JitClosure*>(data);
  if (!cls || cls->arity != 0) bad();
}

// Standalone version of the well-known check for codegen to call only
// when the property name is statically "drop" / "iter" / "next".
// Frees the regular object_set hot path from a name comparison on
// every property bind.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_check_well_known_prop(
    const char* key, int8_t tag, int64_t data) {
  _culebra_check_well_known_prop(key, tag, data);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_set(
    JitObject* obj, const char* key, bool mut, int8_t tag, int64_t data,
    int64_t line, int64_t col) {
  auto idx = obj->find_slot(key);
  if (idx == static_cast<size_t>(-1)) {
    obj->append_slot(key, JitValue{tag, data}, mut);
  } else {
    auto& entry = obj->slots[idx];
    if (!entry.mut) {
      _culebra_value_release_impl(tag, data);
      throw culebra::CulebraError("ImmutableError", std::format(
          "immutable property '{}' at {}:{}.", key, line, col), line, col);
    }
    _culebra_value_release_impl(entry.value.tag, entry.value.data);
    entry.value.tag = tag;
    entry.value.data = data;
  }
  if (std::string_view(key) == "drop") obj->has_drop = true;
}

// Hashable-key access into an Object. String keys unify with the
// shape-based `obj.foo` path so `obj["x"] = v` and `obj.x = v` reach
// the same slot; everything else (Long/Float/Bool/Nil/Tuple) lands in
// the sidecar AnyKeyMap. Object-literal keys are pre-filtered by the
// grammar (no String literal keys), so the TAG_STRING branch only
// fires for runtime-keyed subscript ops.
//
// The shape-path call below passes 0/0 for the IC slot, so a String-
// keyed subscript write never inline-caches. Hot loops should still
// prefer `obj.x = v` over `obj["x"] = v` for that reason.
//
// Refcount contract: this helper consumes the caller's +1 to the key.
// On insert, the +1 transfers to the map's stored alias; the helper
// adds a second +1 for the key_order entry. On update / throw it
// explicitly releases the caller's +1 since the existing stored alias
// already covers the slot.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_set_any(
    JitObject* obj, int8_t key_tag, int64_t key_data, bool mut,
    int8_t val_tag, int64_t val_data) {
  if (key_tag == TAG_STRING) {
    culebra_runtime_object_set(obj, reinterpret_cast<const char*>(key_data),
                               mut, val_tag, val_data, 0, 0);
    return;
  }
  if (!obj->non_string_props) {
    obj->non_string_props = new JitObject::AnyKeyMap();
    // First non-String key: activate key_order and back-fill with the
    // String keys already in `shape->names` so interleaved order survives.
    obj->key_order = new std::vector<JitValue>();
    if (obj->shape) {
      obj->key_order->reserve(obj->shape->names.size() + 1);
      for (const auto& name : obj->shape->names) {
        obj->key_order->push_back(
            {TAG_STRING, reinterpret_cast<int64_t>(name.c_str())});
      }
    }
  }
  JitValue key{key_tag, key_data};
  auto& m = *obj->non_string_props;
  auto it = m.find(key);
  if (it == m.end()) {
    m.emplace(key, JitObjectEntry{JitValue{val_tag, val_data}, mut});
    // Caller's +1 transferred to the map's stored alias. Add a second
    // +1 for the key_order entry.
    culebra_runtime_value_retain(key_tag, key_data);
    obj->key_order->push_back(key);
    return;
  }
  if (!it->second.mut) {
    _culebra_value_release_impl(val_tag, val_data);
    _culebra_value_release_impl(key_tag, key_data);
    throw culebra::CulebraError("ImmutableError",
                                "immutable entry on non-String key");
  }
  _culebra_value_release_impl(it->second.value.tag, it->second.value.data);
  it->second.value.tag = val_tag;
  it->second.value.data = val_data;
  _culebra_value_release_impl(key_tag, key_data);
}

// Consumes the caller's +1 to the key on the refcounted (sidecar)
// path so a Tuple-keyed `obj[k]` read does not leak. TAG_STRING keys
// are non-refcounted (just borrowed cstrings), so they need no release.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_get_any(
    JitObject* obj, int8_t key_tag, int64_t key_data,
    int8_t* out_tag, int64_t* out_data) {
  // String keys: unified with shape access (see object_set_any).
  if (key_tag == TAG_STRING) {
    auto idx = obj->find_slot(reinterpret_cast<const char*>(key_data));
    if (idx == static_cast<size_t>(-1)) {
      throw culebra::CulebraError("KeyError", "key not present");
    }
    *out_tag = obj->slots[idx].value.tag;
    *out_data = obj->slots[idx].value.data;
    culebra_runtime_value_retain(*out_tag, *out_data);
    return;
  }
  if (!obj->non_string_props) {
    _culebra_value_release_impl(key_tag, key_data);
    throw culebra::CulebraError("KeyError", "key not present");
  }
  JitValue key{key_tag, key_data};
  auto it = obj->non_string_props->find(key);
  if (it == obj->non_string_props->end()) {
    _culebra_value_release_impl(key_tag, key_data);
    throw culebra::CulebraError("KeyError", "key not present");
  }
  *out_tag = it->second.value.tag;
  *out_data = it->second.value.data;
  culebra_runtime_value_retain(*out_tag, *out_data);
  _culebra_value_release_impl(key_tag, key_data);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_object_has_any(
    JitObject* obj, int8_t key_tag, int64_t key_data) {
  if (!obj->non_string_props) return 0;
  return obj->non_string_props->contains({key_tag, key_data}) ? 1 : 0;
}

// Fast path for the property-write IC. Caller (JIT-emitted IR) has
// already verified `obj->shape == ic->expected_shape`, so this just
// dispatches on the cached mode:
//   * update (expected == result): overwrite slots[offset], honoring
//     the existing entry's mut flag.
//   * transition (expected != result): grow `slots` by one (the
//     reserved capacity from append_slot's first-call reserve(8)
//     usually means no realloc), bump shape to result_shape.
// `key` is borrowed only for the immutable-error message; never freed.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_set_fast(
    JitObject* obj, const char* key, JitPropSetIC* ic, int8_t tag,
    int64_t data, int64_t line, int64_t col) {
  if (ic->expected_shape == ic->result_shape) {
    auto& entry = obj->slots[ic->offset];
    if (!entry.mut) {
      _culebra_value_release_impl(tag, data);
      throw culebra::CulebraError("ImmutableError", std::format(
          "immutable property '{}' at {}:{}.", key, line, col), line, col);
    }
    _culebra_value_release_impl(entry.value.tag, entry.value.data);
    entry.value.tag = tag;
    entry.value.data = data;
  } else {
    if (obj->slots.capacity() == 0) obj->slots.reserve(8);
    obj->slots.push_back({JitValue{tag, data}, ic->prop_mut != 0});
    obj->shape = static_cast<culebra::Shape*>(ic->result_shape);
    if (obj->key_order) {
      obj->key_order->push_back(
          {TAG_STRING,
           reinterpret_cast<int64_t>(obj->shape->names.back().c_str())});
    }
  }
}

// Slow path for the property-write IC. Performs the full lookup
// (mirrors `culebra_runtime_object_set`), then refreshes the IC so
// subsequent writes at this site with the same starting shape hit
// the fast path. Caches `obj->shape` *as-observed* (including
// nullptr) so fresh-Object writes can hit the fast path on the
// second instance — overwriting it with `root()` would mean a
// permanent miss for objects that always start out with no shape.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_set_ic(
    JitObject* obj, const char* key, JitPropSetIC* ic, bool mut,
    int8_t tag, int64_t data, int64_t line, int64_t col) {
  auto* before = obj->shape;
  auto idx = obj->find_slot(key);
  if (idx == static_cast<size_t>(-1)) {
    auto* base = before ? before : culebra::shape_registry().root();
    auto* result = culebra::shape_registry().transition_add(base, key);
    ic->expected_shape = before;
    ic->result_shape = result;
    ic->offset = result->names.size() - 1;
    ic->prop_mut = mut ? 1 : 0;
    if (obj->slots.capacity() == 0) obj->slots.reserve(8);
    obj->slots.push_back({JitValue{tag, data}, mut});
    obj->shape = result;
    if (obj->key_order) {
      obj->key_order->push_back(
          {TAG_STRING,
           reinterpret_cast<int64_t>(result->names.back().c_str())});
    }
  } else {
    auto& entry = obj->slots[idx];
    if (!entry.mut) {
      _culebra_value_release_impl(tag, data);
      throw culebra::CulebraError("ImmutableError", std::format(
          "immutable property '{}' at {}:{}.", key, line, col), line, col);
    }
    _culebra_value_release_impl(entry.value.tag, entry.value.data);
    entry.value.tag = tag;
    entry.value.data = data;
    ic->expected_shape = before;
    ic->result_shape = before;
    ic->offset = idx;
    ic->prop_mut = entry.mut ? 1 : 0;
  }
  if (std::string_view(key) == "drop") obj->has_drop = true;
}

// Build a structured Error Object for a C++ exception that has reached
// a try/catch landingpad. Called inside an active __cxa_begin_catch
// region so `try { throw; }` re-raises the same exception for type
// inspection. On success, populates the culebra_thrown_* globals as if
// the user had run `throw error_object` and sets `is_throw=1`. Foreign
// exceptions (anything we can't classify) leave `is_throw=0` so the
// caller will propagate them with __cxa_rethrow.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_try_translate() {
  auto& rt = culebra::current_runtime();
  if (rt.is_throw) return;
  auto build = [&rt](std::string_view kind, std::string_view msg,
                     int64_t line, int64_t col) {
    auto* obj = culebra_runtime_object_new();
    culebra_runtime_object_set(
        obj, "kind", false, TAG_STRING,
        reinterpret_cast<int64_t>(_culebra_heap_str(kind)), 0, 0);
    culebra_runtime_object_set(
        obj, "message", false, TAG_STRING,
        reinterpret_cast<int64_t>(_culebra_heap_str(msg)), 0, 0);
    culebra_runtime_object_set(obj, "line", false, TAG_LONG, line, 0, 0);
    culebra_runtime_object_set(obj, "col", false, TAG_LONG, col, 0, 0);
    rt.thrown_tag = TAG_OBJECT;
    rt.thrown_data = reinterpret_cast<int64_t>(obj);
    rt.is_throw = 1;
  };
  try {
    throw;
  } catch (const culebra::CulebraError& e) {
    build(e.kind, e.what(), e.line, e.col);
  } catch (const std::runtime_error& e) {
    build("RuntimeError", e.what(), 0, 0);
  } catch (...) {
    // Foreign exception — let the landingpad rethrow it.
  }
}

// Write `(tag, data)` to the out-params; nil if entry is null.
inline void _write_value_out(const JitObjectEntry* entry, int8_t* out_tag,
                              int64_t* out_data) {
  if (entry) {
    *out_tag = entry->value.tag;
    *out_data = entry->value.data;
  } else {
    *out_tag = 0;
    *out_data = 0;
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_get(
    JitObject* obj, const char* key, int8_t* out_tag, int64_t* out_data) {
  _write_value_out(_find_property(obj, key), out_tag, out_data);
}

// Slow path for the per-callsite IC emitted by compile_property_get.
// On an own-property hit, refreshes `ic->shape` / `ic->offset` so the
// next read with the same shape stays on the inlined fast path. Proto
// hits don't update the cache because the fast path keys on
// `obj->shape`; caching the proto's offset there would load the wrong
// slot. `ic` is borrowed; never released.
// Forward declaration for the trait-default table referenced below.
inline std::unordered_map<std::string,
                          std::unordered_map<std::string, JitClosure*>>&
_jit_trait_default_impls();

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_get_ic(
    JitObject* obj, const char* key, JitPropIC* ic, int8_t* out_tag,
    int64_t* out_data) {
  if (obj->shape) {
    auto idx = obj->shape->offset(key);
    if (idx != static_cast<size_t>(-1)) {
      ic->shape = obj->shape;
      ic->offset = idx;
      _write_value_out(&obj->slots[idx], out_tag, out_data);
      return;
    }
  }
  if (auto* proto_entry = obj->proto ? _find_property(obj->proto, key) : nullptr) {
    _write_value_out(proto_entry, out_tag, out_data);
    return;
  }
  // Trait default-method fallback (T4 part 2). Walk registered
  // defaults; for each candidate trait that owns `key`, check whether
  // this instance conforms (cached). Returns the first matching
  // default's closure as a TAG_FUNC value.
  for (auto& [trait_name, methods] : _jit_trait_default_impls()) {
    auto m_it = methods.find(key);
    if (m_it == methods.end() || !m_it->second) continue;
    if (_culebra_type_matches_single(TAG_OBJECT,
                                      reinterpret_cast<int64_t>(obj),
                                      trait_name)) {
      *out_tag = TAG_FUNC;
      *out_data = reinterpret_cast<int64_t>(m_it->second);
      return;
    }
  }
  *out_tag = TAG_NIL;
  *out_data = 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_object_has(JitObject* obj,
                                                             const char* key) {
  return _find_property(obj, key) != nullptr;
}

// `match v { x: ClassName => ... }` predicate. Returns true when
// `obj` carries a String `class` property whose value equals
// `expected`. Mirrors `culebra::class_tag()` + name comparison in
// the interp's `type_matches`.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_object_class_matches(
    JitObject* obj, const char* expected) {
  auto* entry = _find_property(obj, "class");
  if (!entry || entry->value.tag != TAG_STRING) return false;
  auto* cls = reinterpret_cast<const char*>(entry->value.data);
  return cls && expected && std::strcmp(cls, expected) == 0;
}

// Generic `obj.has(k)`. String keys go through the shape path (matches
// `_find_property`'s proto walk); non-String keys check the sidecar.
//
// Consumes the caller's +1 to the key on the refcounted (sidecar)
// path so a Tuple-keyed `obj.has(k)` does not leak. TAG_STRING data is
// a borrowed cstring (non-refcounted) and needs no release.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_object_has_value(
    JitObject* obj, int8_t tag, int64_t data) {
  if (tag == TAG_STRING) {
    auto* cstr = reinterpret_cast<const char*>(data);
    return _find_property(obj, cstr) != nullptr;
  }
  bool result = obj->non_string_props
                    ? obj->non_string_props->contains({tag, data})
                    : false;
  _culebra_value_release_impl(tag, data);
  return result;
}

// Build a "class meta" object that holds shared method closures for
// proto delegation. Called once per class declaration (compile-time
// emission, runtime allocation), captured in the constructor closure
// so each instance can point its `proto` at the same meta.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_build_class_meta(
    const char* const* method_names, const JitValue* method_vals,
    int64_t n_methods) {
  auto* meta = culebra_runtime_object_new();
  for (int64_t i = 0; i < n_methods; i++) {
    culebra_runtime_value_retain(method_vals[i].tag, method_vals[i].data);
    culebra_runtime_object_set(meta, method_names[i], /*mut*/ false,
                               method_vals[i].tag, method_vals[i].data, 0,
                               0);
  }
  // The meta isn't an instance; undo the `has_drop` side-effect that
  // `culebra_runtime_object_set` set when binding the "drop" method.
  meta->has_drop = false;
  return meta;
}

// Class-sugar constructor body. Mirrors the tree interpreter's
// `eval_class_decl` constructor path:
//   1. Allocate a fresh instance (refcount=1, caller-owned).
//   2. Stamp the `class:` tag.
//   3. Wire the instance's `proto` at the shared class meta object;
//      method lookups fall through to it via _lookup_special /
//      culebra_runtime_object_get / object_has.
//   4. Invoke the user's `new` body (if any) with `this` bound to the
//      new instance. Args are forwarded with +1 ownership intact
//      (JIT ABI); the body's return value is discarded.
//   5. Promote every own property to mutable so later `this.x = y`
//      calls don't trip the immutable-property guard.
//
// `body_tag`/`body_data` describe the user-body closure (TAG_FUNC) or
// TAG_NIL for a class with no `new` method. `class_meta` is borrowed;
// this helper retains it once per instance to balance the matching
// release in the Object destructor.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_build_class_instance(
    const char* class_name, JitObject* class_meta, int8_t body_tag,
    int64_t body_data, int64_t n_args, JitValue* args) {
  auto* inst = culebra_runtime_object_new();

  inst->proto = class_meta;
  // Retain the meta on the instance so it lives at least as long as
  // any of its instances. The matching release runs in the JitObject
  // destructor (release_impl GC_TAG_OBJECT path).
  if (class_meta) class_meta->refcount++;
  // Mirror inherited `drop` so the destructor's `has_drop` gate fires.
  if (class_meta && _find_property(class_meta, "drop")) inst->has_drop = true;

  // `class_name` is a process-lifetime LLVM module global; TAG_STRING
  // values are borrowed (no refcount), so we can stash it directly
  // without the per-instance malloc + memcpy that `_culebra_heap_str`
  // would do.
  culebra_runtime_object_set(inst, "class", /*mut*/ false, TAG_STRING,
                             reinterpret_cast<int64_t>(class_name), 0, 0);

  if (body_tag == TAG_FUNC) {
    auto* body_cls = reinterpret_cast<JitClosure*>(body_data);
    JitValue this_val = {TAG_OBJECT, reinterpret_cast<int64_t>(inst)};
    // Body consumes `this` via the slot +1 on function exit, so retain
    // before handing it off. If the body throws, the instance's +1 is
    // otherwise stranded — release it before rethrowing to the caller.
    culebra_runtime_value_retain(this_val.tag, this_val.data);
    try {
      auto result = reinterpret_cast<JitFn>(body_cls->fn_ptr)(
          body_cls, this_val, n_args, args);
      _culebra_value_release_impl(result.tag, result.data);
    } catch (...) {
      _culebra_value_release_impl(TAG_OBJECT,
                                   reinterpret_cast<int64_t>(inst));
      throw;
    }
  } else {
    // Default constructor: release any args the caller transferred.
    for (int64_t i = 0; i < n_args; i++) {
      _culebra_value_release_impl(args[i].tag, args[i].data);
    }
  }

  for (auto& entry : inst->slots) {
    entry.mut = true;
  }

  return {TAG_OBJECT, reinterpret_cast<int64_t>(inst)};
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_object_size(
    JitObject* obj) {
  int64_t n = static_cast<int64_t>(obj->prop_size());
  if (obj->non_string_props) {
    n += static_cast<int64_t>(obj->non_string_props->size());
  }
  return n;
}

// --- Multimethod dispatch (interp-parity, see eval_multifn_decl) ---
//
// The two tables below are process-wide statics, mirroring the
// single-threaded scoping of `culebra_thrown_tag` etc. (see the
// runtime-globals comment near the top of this file). Multiple JIT
// instances in the same process — and successive runs that share a
// process — accumulate entries; if that becomes a real configuration
// (e.g. a long-lived host embedding repeated culebra invocations) we
// should switch to a per-JitContext registry. For the supported
// single-binary single-run case, both tables are bounded by the
// number of multimethods declared in the script and are torn down at
// process exit alongside the closures they reference.

// Forward declarations — closure_new and value_release_impl are
// defined later in this file but used by the multimethod runtime.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitClosure* culebra_runtime_closure_new(
    void* fn_ptr, size_t n_captures, size_t arity);

// Parameter metadata attached to JIT-compiled user functions. The side
// table below is keyed by `fn_ptr` (the underlying JitFn pointer) and
// populated at JIT-module init time for each FUNCTION literal. It lets
// `culebra_runtime_call_with_kwargs` resolve names → slab positions
// at runtime — supporting indirect callees (captures, method values,
// UFCS) and dynamic `**variable` splats. Built-in closures created by
// `culebra_runtime_closure_new` from runtime helpers (iter wrappers
// etc.) never register meta and so reject kwargs.
struct JitParamMeta {
  const char* const* names;  // pointers into module-level globals
  // Bit i is set if param i has a default expression. The callee
  // prologue handles defaults inline; the resolver only uses this to
  // decide whether an unfilled middle slot is OK (default) or an
  // ArityError (required).
  const uint8_t* has_default_bits;
  size_t n_params;
  // Index of the `**rest` catch-all parameter, or -1 if none. The
  // runtime resolver builds an Object from unconsumed kwargs and
  // places it in slab[kwargs_rest_idx].
  int64_t kwargs_rest_idx;
  // Index of the first keyword-only parameter (after a `*` separator
  // with regular params before it), or -1. Used by dynamic-callee
  // dispatch to enforce kw-only at runtime when a closure is captured
  // and called positionally (`let g = f; g(1, 2)` where f is kw-only).
  int64_t first_kw_only_idx;
  // Introspection-only fields appended at the end so existing kwargs
  // dispatch consumers stay unaffected. `fn_name`/`return_type` are
  // always non-null cstrings (empty string if unset). `mut_bits` is a
  // bitmap (one bit per param). `type_names` is an array of n_params
  // cstrings (each non-null, "" if unannotated).
  const char* fn_name;
  const char* return_type;
  const uint8_t* mut_bits;
  const char* const* type_names;
  // Original declared annotation per param (before class type-param
  // neutralization). Used by introspection so `fn.params[i].type`
  // surfaces `T` / `Array<T>` for Generic class methods. Same layout
  // as `type_names`; empty string when unannotated.
  const char* const* declared_type_names;
};

// Layout matches the LLVM struct emitted in emit_param_meta_global.
// Adding a field requires updating both — the assert catches drift.
static_assert(sizeof(JitParamMeta) == 10 * sizeof(int64_t),
              "JitParamMeta C++ / LLVM layout drift");

inline std::unordered_map<void*, const JitParamMeta*>&
_jit_param_meta_table() {
  static std::unordered_map<void*, const JitParamMeta*> tbl;
  return tbl;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_register_param_meta(
    void* fn_ptr, const JitParamMeta* meta) {
  _jit_param_meta_table()[fn_ptr] = meta;
}

inline const JitParamMeta* _jit_lookup_param_meta(void* fn_ptr) {
  auto& tbl = _jit_param_meta_table();
  auto it = tbl.find(fn_ptr);
  return it == tbl.end() ? nullptr : it->second;
}

// Enforce kw-only at runtime for dynamic-callee positional calls.
// `let g = f; g(1, 2)` where f is kw-only would otherwise fill the
// kw-only slot positionally without the compile-time static check
// firing — this runtime guard catches that case to match the interp.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_check_pos_count(
    void* fn_ptr, int64_t n_pos, int64_t line, int64_t col) {
  const JitParamMeta* meta = _jit_lookup_param_meta(fn_ptr);
  if (!meta) return;
  culebra::throw_if_too_many_positionals(meta->first_kw_only_idx, n_pos,
                                          line, col);
}

struct JitMultiMethodEntry {
  std::vector<std::string> param_types;  // empty = "Any"
  JitClosure* body;                       // +1 owned by this entry
};

inline std::map<std::string, std::vector<JitMultiMethodEntry>>&
_jit_multimethods() {
  static std::map<std::string, std::vector<JitMultiMethodEntry>> tbl;
  return tbl;
}

// Trait default-method bodies on the JIT side. Outer map keyed by
// trait name, inner by method name. Holds a +1 reference on each
// closure for the program's lifetime.
inline std::unordered_map<std::string,
                          std::unordered_map<std::string, JitClosure*>>&
_jit_trait_default_impls() {
  static std::unordered_map<std::string,
                            std::unordered_map<std::string, JitClosure*>> tbl;
  return tbl;
}

// Runtime registration hook called from compile_trait_decl's emitted
// IR. The compiled default-method JitValue is unpacked at the call
// site; we receive the closure pointer plus the trait/method names
// as process-lifetime cstrings (module-level globals).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_register_trait_default(const char* trait_name,
                                        const char* method_name,
                                        JitClosure* closure) {
  if (!closure) return;
  auto& slot = _jit_trait_default_impls()[trait_name][method_name];
  if (slot) {
    _culebra_value_release_impl(TAG_FUNC, reinterpret_cast<int64_t>(slot));
  }
  closure->refcount++;
  slot = closure;
}

// Add (or refresh) a method entry on the registered trait. Called
// once per declared method from compile_trait_decl's emitted IR so
// trait_registry is populated at AOT-binary runtime (and at JIT
// runtime — process-global registry survives the JIT phase but AOT
// has a fresh process).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_register_trait_method(const char* trait_name,
                                       const char* method_name,
                                       int64_t arity,
                                       int8_t has_default) {
  std::unique_lock lock(culebra::trait_mutex());
  auto& reg = culebra::trait_registry();
  std::string name(trait_name);
  auto& def = reg[name];
  if (def.name.empty()) def.name = name;
  // Replace existing entry by method name (REPL re-declaration safety).
  for (auto& m : def.methods) {
    if (m.name == method_name) {
      m.arity = static_cast<size_t>(arity);
      m.has_default = has_default != 0;
      culebra::trait_conformance_cache().clear();
      return;
    }
  }
  def.methods.push_back({std::string(method_name),
                          static_cast<size_t>(arity),
                          has_default != 0});
  culebra::trait_conformance_cache().clear();
}

// Side table mapping a dispatcher closure pointer to its multimethod
// name, so the shared static thunk can recover which name to dispatch
// for. Dispatchers live for the lifetime of the program (held by the
// env binding); leak is bounded by the number of multimethods.
inline std::map<JitClosure*, std::string>&
_jit_multifn_dispatcher_names() {
  static std::map<JitClosure*, std::string> tbl;
  return tbl;
}

// Function-value introspection. `cls` is a JitClosure*; `prop` is
// one of "name" / "return_type" / "params". Mirrors the interp side's
// Value::eval_property dispatch for Value::Function. Unknown
// properties return Nil. Used by compile_property_get when the
// receiver tag is TAG_FUNC.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue
culebra_runtime_fn_introspect_get(JitClosure* cls, const char* prop) {
  const JitParamMeta* meta =
      cls ? _jit_lookup_param_meta(cls->fn_ptr) : nullptr;
  // Multifn dispatcher fallback: dispatchers share a single thunk
  // address, so per-name body meta isn't keyed by fn_ptr. Walk the
  // dispatcher→name registry to find the body's meta — surfaces the
  // first registered method's signature (interp parity with the
  // dispatcher exposing the first method's params/name/return_type).
  if (!meta && cls) {
    auto& dispatchers = _jit_multifn_dispatcher_names();
    auto disp_it = dispatchers.find(cls);
    if (disp_it != dispatchers.end()) {
      auto& tbl = _jit_multimethods();
      auto method_it = tbl.find(disp_it->second);
      if (method_it != tbl.end() && !method_it->second.empty()) {
        meta = _jit_lookup_param_meta(method_it->second.front().body->fn_ptr);
      }
    }
  }
  if (std::strcmp(prop, "name") == 0) {
    const char* n = (meta && meta->fn_name) ? meta->fn_name : "";
    auto* heap = _culebra_heap_str(std::string(n));
    return {TAG_STRING, reinterpret_cast<int64_t>(heap)};
  }
  if (std::strcmp(prop, "return_type") == 0) {
    const char* r = (meta && meta->return_type) ? meta->return_type : "";
    auto* heap = _culebra_heap_str(culebra::canonicalize_type_annotation(r));
    return {TAG_STRING, reinterpret_cast<int64_t>(heap)};
  }
  if (std::strcmp(prop, "params") == 0) {
    auto* arr = culebra_runtime_array_new();
    if (meta) {
      for (size_t i = 0; i < meta->n_params; i++) {
        auto* o = culebra_runtime_object_new();
        auto* nname = _culebra_heap_str(std::string(meta->names[i]));
        culebra_runtime_object_set(o, "name", false, TAG_STRING,
                                    reinterpret_cast<int64_t>(nname), 0, 0);
        bool m = meta->mut_bits &&
                 (meta->mut_bits[i / 8] & (1u << (i % 8)));
        culebra_runtime_object_set(o, "mut", false, TAG_BOOL,
                                    m ? 1 : 0, 0, 0);
        // Prefer declared (preserves `T`); fall back to effective.
        const char* tn = nullptr;
        if (meta->declared_type_names && meta->declared_type_names[i] &&
            meta->declared_type_names[i][0] != '\0') {
          tn = meta->declared_type_names[i];
        } else if (meta->type_names && meta->type_names[i]) {
          tn = meta->type_names[i];
        }
        auto* tname = _culebra_heap_str(
            culebra::canonicalize_type_annotation(tn ? tn : ""));
        culebra_runtime_object_set(o, "type", false, TAG_STRING,
                                    reinterpret_cast<int64_t>(tname), 0, 0);
        bool hd = meta->has_default_bits[i / 8] & (1u << (i % 8));
        culebra_runtime_object_set(o, "has_default", false, TAG_BOOL,
                                    hd ? 1 : 0, 0, 0);
        bool kr = meta->kwargs_rest_idx >= 0 &&
                  static_cast<int64_t>(i) == meta->kwargs_rest_idx;
        // kwargs_rest is reported via its dedicated flag — exclude it
        // from kw_only to match interp, which constructs the **rest
        // Parameter with kw_only=false regardless of separator order.
        bool ko = !kr && meta->first_kw_only_idx >= 0 &&
                  static_cast<int64_t>(i) >= meta->first_kw_only_idx;
        culebra_runtime_object_set(o, "kw_only", false, TAG_BOOL,
                                    ko ? 1 : 0, 0, 0);
        culebra_runtime_object_set(o, "kwargs_rest", false, TAG_BOOL,
                                    kr ? 1 : 0, 0, 0);
        culebra_runtime_array_push(arr, TAG_OBJECT,
                                    reinterpret_cast<int64_t>(o));
      }
    }
    return {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
  }
  return {TAG_NIL, 0};
}

// Persistent top-level binding storage for the JIT REPL. Each entry
// owns +1 on its stored JitValue; the destructor drops every ref so
// REPL session teardown leaves the runtime clean.
//
// Variables defined at REPL top-level (`let x = expr`, `mut y = ...`,
// `fn name(...) {...}`) compile to `culebra_runtime_repl_set` against
// this dict; references emit `culebra_runtime_repl_get`. Function
// bodies and nested blocks keep using the SSA/cell machinery — only
// the REPL-prompt boundary touches this map.
struct JitReplGlobals {
  std::vector<std::string> order;   // insertion order
  std::unordered_map<std::string, JitValue> by_name;
  std::unordered_map<std::string, bool> is_mut;

  ~JitReplGlobals() {
    for (auto& [_, v] : by_name) {
      _culebra_value_release_impl(v.tag, v.data);
    }
  }
};

// Thread-local active REPL globals dict. `run_repl` sets this before
// invoking the input function and clears it on return; runtime
// helpers and any JIT-compiled function body — top-level or nested
// closure — consult it without needing the dict threaded through
// their signature. One REPL session per thread.
inline thread_local JitReplGlobals* _jit_repl_globals_current = nullptr;

// Out-parameter style avoids any ABI surprise around 16-byte struct
// return on ARM64 macOS (where LLVM's regs-vs-sret choice for
// `{i8, i64}` didn't agree with the C++ side for this entry).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_repl_get(const char* name,
                          int64_t line, int64_t col,
                          int8_t* out_tag, int64_t* out_data) {
  auto* g = _jit_repl_globals_current;
  if (!g) {
    throw culebra::CulebraError("NameError",
        std::format("undefined variable '{}'", name), line, col);
  }
  auto it = g->by_name.find(name);
  if (it == g->by_name.end()) {
    throw culebra::CulebraError("NameError",
        std::format("undefined variable '{}'", name), line, col);
  }
  culebra_runtime_value_retain(it->second.tag, it->second.data);
  *out_tag = it->second.tag;
  *out_data = it->second.data;
}

// Caller transfers +1 on (tag, data). On overwrite of an existing
// (mutable) binding the previous +1 is released here. Re-binding via
// `let` is allowed for any name (matches interp REPL shadow semantics);
// `mut`-vs-immutable enforcement only kicks in for plain assignment.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_repl_set(const char* name,
                          int8_t tag, int64_t data,
                          int8_t is_let, int8_t is_mut,
                          int64_t line, int64_t col) {
  auto* g = _jit_repl_globals_current;
  if (!g) {
    _culebra_value_release_impl(tag, data);
    throw culebra::CulebraError("RuntimeError",
        "repl_set called outside REPL session", line, col);
  }
  auto it = g->by_name.find(name);
  if (it != g->by_name.end()) {
    if (!is_let) {
      // Plain assignment: enforce mutability.
      auto m = g->is_mut.find(name);
      if (m == g->is_mut.end() || !m->second) {
        _culebra_value_release_impl(tag, data);
        culebra::throw_immutable_assign_at(name, line, col);
      }
    }
    _culebra_value_release_impl(it->second.tag, it->second.data);
    it->second = {tag, data};
    if (is_let) g->is_mut[name] = static_cast<bool>(is_mut);
  } else {
    g->order.push_back(name);
    g->by_name.emplace(name, JitValue{tag, data});
    g->is_mut.emplace(name, static_cast<bool>(is_mut));
  }
}

// Runtime type name for dispatch. For primitives this is the same as
// `_culebra_tag_name`, but for class instances (TAG_OBJECT with a
// `class:` String slot) we substitute the class name — that's why
// this can't share `_culebra_tag_name`'s tag-only switch. Mirrors
// `value_dyn_type` in interpreter.h.
inline std::string_view _jit_value_dyn_type(JitValue v) {
  switch (v.tag) {
    case TAG_NIL:        return "Nil";
    case TAG_BOOL:       return "Bool";
    case TAG_LONG:       return "Long";
    case TAG_FLOAT:      return "Float";
    case TAG_STRING:     return "String";
    case TAG_STRINGVIEW: return "StringView";
    case TAG_ARRAY:      return "Array";
    case TAG_TUPLE:      return "Tuple";
    case TAG_SET:        return "Set";
    case TAG_FUNC:       return "Function";
    case TAG_TENSOR:     return "Tensor";
    case TAG_OBJECT: {
      auto* obj = reinterpret_cast<JitObject*>(v.data);
      if (auto idx = obj->find_slot("class");
          idx != static_cast<size_t>(-1)) {
        const auto& slot = obj->slots[idx].value;
        if (slot.tag == TAG_STRING) {
          return std::string_view(reinterpret_cast<const char*>(slot.data));
        }
      }
      return "Object";
    }
  }
  return "Object";
}

// Adapter: supplies the param-types accessor for the shared pick
// algorithm in interpreter.h.
inline int64_t _jit_multifn_pick(
    const std::vector<JitMultiMethodEntry>& methods,
    const std::vector<std::string_view>& arg_types) {
  return culebra::multifn_pick(
      methods, arg_types,
      [](const JitMultiMethodEntry& m) -> const std::vector<std::string>& {
        return m.param_types;
      });
}

// Recover (body, name) for a multifn dispatcher closure given the
// positional arg types. Throws DispatchError on no-match / ambiguous;
// `release` runs before every throw, letting the kwargs-path caller
// drop the +1s it owns. Returns the picked method's body closure.
struct MultifnPick {
  JitClosure* body;
  std::string_view name;
};
inline MultifnPick _jit_multifn_resolve(
    JitClosure* cls, JitValue* positional, int64_t n_pos,
    int64_t line, int64_t col,
    std::function<void()> release = nullptr) {
  auto name_it = _jit_multifn_dispatcher_names().find(cls);
  if (name_it == _jit_multifn_dispatcher_names().end()) {
    if (release) release();
    throw std::runtime_error("internal: multimethod dispatcher missing name");
  }
  auto& tbl = _jit_multimethods();
  auto m_it = tbl.find(name_it->second);
  auto fail = [&](const char* what) -> std::string {
    if (release) release();
    return std::format("{} for `{}`", what, name_it->second);
  };
  if (m_it == tbl.end()) {
    throw culebra::CulebraError("DispatchError",
        fail("no matching method"), line, col);
  }
  std::vector<std::string_view> arg_types(static_cast<size_t>(n_pos));
  for (size_t i = 0; i < arg_types.size(); i++) {
    arg_types[i] = _jit_value_dyn_type(positional[i]);
  }
  // Pre-populate trait conformance cache so multifn_specificity can
  // score `fn show(x: Stringer)` style trait params without holding
  // the arg instances. Mirrors interp's pick_method warm-up.
  for (const auto& trait_name : culebra::snapshot_trait_names()) {
    for (size_t i = 0; i < arg_types.size(); i++) {
      (void)_culebra_type_matches_single(positional[i].tag,
                                          positional[i].data, trait_name);
    }
  }
  auto pick = _jit_multifn_pick(m_it->second, arg_types);
  if (pick == -1) {
    throw culebra::CulebraError("DispatchError",
        fail("no matching method"), line, col);
  }
  if (pick == -2) {
    throw culebra::CulebraError("DispatchError",
        fail("ambiguous dispatch"), line, col);
  }
  return {m_it->second[static_cast<size_t>(pick)].body, name_it->second};
}

// JitFn-ABI shared thunk installed as `fn_ptr` on every multimethod
// dispatcher closure. The `cls` identity (load-bearing comparison in
// `culebra_runtime_call_with_kwargs` to intercept the kwargs path) is
// resolved through `_jit_multifn_dispatcher_names()` to recover the
// multimethod name, then dispatched via `_jit_multifn_resolve`.
inline JitValue _jit_multifn_dispatcher_thunk(JitClosure* cls,
                                              JitValue /*this_val*/,
                                              int64_t n_args,
                                              JitValue* args) {
  auto picked = _jit_multifn_resolve(cls, args, n_args, 0, 0);
  return reinterpret_cast<JitFn>(picked.body->fn_ptr)(
      picked.body, {TAG_NIL, 0}, n_args, args);
}

// Append a method to the named multimethod table (replacing an entry
// with an identical param-type sequence — REPL / re-decl semantics).
// Returns the dispatcher closure for `name`, creating it on first
// call and caching it across re-decls. Caller takes a +1 reference
// (the env binding) and is responsible for the matching release.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitClosure*
culebra_runtime_multifn_register_and_install(const char* name_cstr,
                                             JitClosure* body,
                                             const char* const* param_types,
                                             int64_t n_param_types) {
  std::string name(name_cstr);
  JitMultiMethodEntry method;
  // Caller's `body` arrives at +1; the table takes that ownership
  // outright. On replace, the displaced body's +1 is released below.
  method.body = body;
  method.param_types.reserve(static_cast<size_t>(n_param_types));
  for (int64_t i = 0; i < n_param_types; i++) {
    // Canonicalize so `Long|Float` and `Long | Float` dedup to one entry.
    method.param_types.emplace_back(culebra::canonicalize_type_annotation(
        param_types[i] ? param_types[i] : ""));
  }

  auto& tbl = _jit_multimethods();
  auto& methods = tbl[name];
  bool replaced = false;
  for (auto& existing : methods) {
    if (existing.param_types == method.param_types) {
      _culebra_value_release_impl(
          TAG_FUNC, reinterpret_cast<int64_t>(existing.body));
      existing = std::move(method);
      replaced = true;
      break;
    }
  }
  if (!replaced) methods.push_back(std::move(method));

  // Find or create the dispatcher closure for this name.
  for (auto& [cls_ptr, n] : _jit_multifn_dispatcher_names()) {
    if (n == name) {
      cls_ptr->refcount++;  // hand a +1 back to the caller
      return cls_ptr;
    }
  }
  auto* dispatcher = culebra_runtime_closure_new(
      reinterpret_cast<void*>(&_jit_multifn_dispatcher_thunk),
      /*n_captures=*/0, /*arity=*/n_param_types);
  _jit_multifn_dispatcher_names()[dispatcher] = name;
  // closure_new returns +1; caller becomes the owner. Don't bump again.
  return dispatcher;
}

// Auto-synthesized `class.parameters()` walker, mirroring the interp
// helper in interpreter.h::_walk_collect_params. Walks `val`
// recursively: Arrays are descended element-wise, plain Object dicts
// (no `class:` tag) are descended into their property values, and
// class instances are collected as leaves. Scalars are skipped.
// Property keys starting with '_' are skipped (private/cache fields).
// Each collected leaf is appended to `out` with its refcount bumped
// (+1 retained); the caller owns `out`'s eventual release.
inline void _jit_walk_collect_params(JitValue v, JitArray* out);

inline void _jit_walk_collect_params_object(JitObject* obj, JitArray* out) {
  // Both backends iterate Object properties in insertion order. The
  // Shape's names vector already stores them in declaration order.
  if (!obj->shape) return;
  for (size_t i = 0; i < obj->shape->names.size(); i++) {
    std::string_view key(obj->shape->names[i]);
    if (key == "class") continue;
    if (!key.empty() && key[0] == '_') continue;
    _jit_walk_collect_params(obj->slots[i].value, out);
  }
}

inline void _jit_walk_collect_params(JitValue v, JitArray* out) {
  if (v.tag == TAG_ARRAY) {
    auto* arr = reinterpret_cast<JitArray*>(v.data);
    for (size_t i = 0; i < arr->size; i++) {
      _jit_walk_collect_params(arr->items[i], out);
    }
  } else if (v.tag == TAG_OBJECT) {
    auto* obj = reinterpret_cast<JitObject*>(v.data);
    if (obj->has_own("class")) {
      // Class instance — leaf parameter, collect.
      culebra_runtime_value_retain(v.tag, v.data);
      culebra_runtime_array_push(out, v.tag, v.data);
    } else {
      // Plain Object dict — recurse into values.
      _jit_walk_collect_params_object(obj, out);
    }
  }
}

// Entry point invoked from JIT IR for `model.parameters()` when the
// receiver is a class instance and has no user-defined `parameters`
// method. Returns a fresh +1 Array of class-instance Values.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_class_parameters_walk(
    JitObject* obj) {
  auto* result = culebra_runtime_array_new();
  _jit_walk_collect_params_object(obj, result);
  return {TAG_ARRAY, reinterpret_cast<int64_t>(result)};
}

// --- Cell runtime ---

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitCell* culebra_runtime_cell_new(int8_t tag,
                                                               int64_t data) {
  auto* c = new JitCell();
  c->refcount = 1;
  c->value.tag = tag;
  c->value.data = data;
  _gc().add(c, GC_TAG_CELL);
  return c;
}

// --- Closure runtime ---

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitClosure* culebra_runtime_closure_new(
    void* fn_ptr, size_t n_captures, size_t arity) {
  auto* c = new JitClosure();
  c->refcount = 1;
  c->fn_ptr = fn_ptr;
  c->n_captures = n_captures;
  c->captures =
      n_captures ? static_cast<JitCell**>(
                       std::malloc(sizeof(JitCell*) * n_captures))
                 : nullptr;
  c->arity = arity;
  _gc().add(c, GC_TAG_FUNC);
  return c;
}

// Runtime kwarg resolver: routes a call carrying keyword arguments
// and/or dynamic `**splat` operands against a closure whose parameter
// names live in the JitParamMeta side table. Mirrors the interp's
// `bind_call_args` algorithm — splats merge first (later wins),
// explicit kwargs layer on top. Missing defaulted slots get the
// `TAG_UNFILLED` sentinel; the callee prologue's existing default
// branch picks them up. The slab also carries any overflow positional
// args past the formal arity so `__ARGS__` continues to work.
//
// Refcount contract: caller transfers +1 on each positional, kwargs
// value, and splat Object. All transferred values either flow into
// the dispatched call (consumed by the callee frame) or are released
// here on the throw paths.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_call_with_kwargs(
    JitClosure* cls, JitValue this_val,
    int64_t n_pos, JitValue* positional,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs,
    int64_t line, int64_t col) {
  auto release_owned = [&](size_t skip_pos = 0) {
    // Drop every +1 the caller transferred to us that hasn't already
    // been consumed downstream. `skip_pos` lets us skip positionals
    // that have already been moved into a slab cell about to be
    // handed to the callee.
    for (int64_t i = static_cast<int64_t>(skip_pos); i < n_pos; i++) {
      _culebra_value_release_impl(positional[i].tag, positional[i].data);
    }
    for (int64_t i = 0; i < n_kw; i++) {
      _culebra_value_release_impl(kw_vals[i].tag, kw_vals[i].data);
    }
    for (int64_t i = 0; i < n_splat; i++) {
      _culebra_value_release_impl(splat_objs[i].tag, splat_objs[i].data);
    }
  };

  // Multifn dispatchers route through here too: their fn_ptr has no
  // JitParamMeta (kwargs flow on top of the existing positional pick,
  // Julia kwsorter style). Pick on positional types, then recurse
  // into the picked method — which is a regular user fn, never a
  // dispatcher, so the recursion bottoms out in the meta-lookup
  // branch below.
  if (cls->fn_ptr == reinterpret_cast<void*>(&_jit_multifn_dispatcher_thunk)) {
    auto picked = _jit_multifn_resolve(
        cls, positional, n_pos, line, col, release_owned);
    return culebra_runtime_call_with_kwargs(
        picked.body, this_val, n_pos, positional, n_kw, kw_keys, kw_vals,
        n_splat, splat_objs, line, col);
  }

  const JitParamMeta* meta = _jit_lookup_param_meta(cls->fn_ptr);
  if (!meta) {
    release_owned();
    throw culebra::CulebraError("TypeError",
        "function does not accept keyword arguments", line, col);
  }

  // Validate splat operands and merge each Object's String-keyed
  // entries into a single name → JitValue map.
  std::unordered_map<std::string_view, JitValue> merged;
  for (int64_t i = 0; i < n_splat; i++) {
    auto sv = splat_objs[i];
    if (sv.tag != TAG_OBJECT) {
      release_owned();
      throw culebra::CulebraError("TypeError", std::format(
          "**: splat operand must be Object, got {}",
          _culebra_tag_name(sv.tag)), line, col);
    }
    auto* obj = reinterpret_cast<JitObject*>(sv.data);
    if (obj->non_string_props && !obj->non_string_props->empty()) {
      release_owned();
      throw culebra::CulebraError("TypeError",
          "**: splat key must be String", line, col);
    }
    if (!obj->shape) continue;
    for (size_t k = 0; k < obj->shape->names.size(); k++) {
      // Retain each value so the merged map owns +1; the matching
      // release lands either on the slab transfer or in the catch
      // paths below.
      auto& sv_entry = obj->slots[k].value;
      culebra_runtime_value_retain(sv_entry.tag, sv_entry.data);
      auto it = merged.find(obj->shape->names[k]);
      if (it != merged.end()) {
        _culebra_value_release_impl(it->second.tag, it->second.data);
        it->second = sv_entry;
      } else {
        merged.emplace(obj->shape->names[k], sv_entry);
      }
    }
  }

  // Layer explicit kwargs on top (overwriting any splat-contributed
  // binding for the same key; duplicate among explicit names was
  // already rejected at the IR scan).
  for (int64_t i = 0; i < n_kw; i++) {
    auto it = merged.find(kw_keys[i]);
    if (it != merged.end()) {
      _culebra_value_release_impl(it->second.tag, it->second.data);
      it->second = kw_vals[i];
    } else {
      merged.emplace(kw_keys[i], kw_vals[i]);
    }
  }

  // Build resolved slab: one entry per formal param, plus extras.
  // Required slots must be filled by positional or merged kwargs.
  size_t arity = meta->n_params;
  size_t n_extras = (n_pos > static_cast<int64_t>(arity))
                        ? static_cast<size_t>(n_pos) - arity
                        : 0;
  std::vector<JitValue> slab(arity + n_extras);
  std::vector<bool> filled(arity, false);

  for (size_t i = 0; i < arity && i < static_cast<size_t>(n_pos); i++) {
    auto it = merged.find(meta->names[i]);
    if (it != merged.end()) {
      _culebra_value_release_impl(it->second.tag, it->second.data);
      merged.erase(it);
      // Release everything we still own and the splat refs we already
      // dropped via release_owned were not in scope yet.
      for (size_t k = 0; k < i; k++) {
        _culebra_value_release_impl(slab[k].tag, slab[k].data);
      }
      for (size_t k = i; k < static_cast<size_t>(n_pos); k++) {
        _culebra_value_release_impl(positional[k].tag, positional[k].data);
      }
      for (auto& [_, v] : merged) {
        _culebra_value_release_impl(v.tag, v.data);
      }
      for (int64_t k = 0; k < n_splat; k++) {
        _culebra_value_release_impl(splat_objs[k].tag, splat_objs[k].data);
      }
      throw culebra::CulebraError("TypeError",
          std::format("got argument '{}' both positionally and as a "
                      "keyword", meta->names[i]), line, col);
    }
    slab[i] = positional[i];
    filled[i] = true;
  }

  for (size_t i = static_cast<size_t>(n_pos); i < arity; i++) {
    if (static_cast<int64_t>(i) == meta->kwargs_rest_idx) continue;
    auto it = merged.find(meta->names[i]);
    if (it != merged.end()) {
      slab[i] = it->second;
      filled[i] = true;
      merged.erase(it);
    }
  }

  // Leftover kwargs flow into a `**rest` slot if declared, otherwise
  // they are unknown to this function. The rest slot is always
  // populated (even with an empty Object) so the callee always sees
  // a bound variable.
  if (meta->kwargs_rest_idx >= 0) {
    auto* rest_obj = culebra_runtime_object_new();
    for (auto& [k, v] : merged) {
      culebra_runtime_object_set(rest_obj, std::string(k).c_str(),
                                  /*mut=*/false, v.tag, v.data, line, col);
    }
    merged.clear();
    slab[meta->kwargs_rest_idx] = {TAG_OBJECT,
                                    reinterpret_cast<int64_t>(rest_obj)};
    filled[meta->kwargs_rest_idx] = true;
  } else if (!merged.empty()) {
    auto bad_name = std::string(merged.begin()->first);
    for (auto& [_, v] : merged) {
      _culebra_value_release_impl(v.tag, v.data);
    }
    for (size_t i = 0; i < slab.size(); i++) {
      if (i < arity && !filled[i]) continue;
      _culebra_value_release_impl(slab[i].tag, slab[i].data);
    }
    for (int64_t i = 0; i < n_splat; i++) {
      _culebra_value_release_impl(splat_objs[i].tag, splat_objs[i].data);
    }
    throw culebra::CulebraError("TypeError",
        std::format("unknown keyword argument '{}'", bad_name),
        line, col);
  }

  // Validate required slots are filled. Defaulted slots get
  // TAG_UNFILLED so the callee prologue takes the default branch.
  for (size_t i = 0; i < arity; i++) {
    if (filled[i]) continue;
    bool defaulted =
        meta->has_default_bits[i / 8] & (1u << (i % 8));
    if (!defaulted) {
      auto missing = meta->names[i];
      for (size_t k = 0; k < arity; k++) {
        if (filled[k]) _culebra_value_release_impl(slab[k].tag,
                                                    slab[k].data);
      }
      for (int64_t k = 0; k < n_splat; k++) {
        _culebra_value_release_impl(splat_objs[k].tag, splat_objs[k].data);
      }
      throw culebra::CulebraError("ArityError",
          std::format("missing required argument '{}'", missing),
          line, col);
    }
    slab[i] = {TAG_UNFILLED, 0};
  }

  // Extras past the formal arity flow into `__ARGS__`.
  for (size_t i = 0; i < n_extras; i++) {
    slab[arity + i] = positional[arity + i];
  }

  // Splat Objects are no longer needed: their values were retained on
  // entry to merged and either consumed into the slab or released
  // above. Drop our +1 to each Object itself.
  for (int64_t i = 0; i < n_splat; i++) {
    _culebra_value_release_impl(splat_objs[i].tag, splat_objs[i].data);
  }

  return reinterpret_cast<JitFn>(cls->fn_ptr)(
      cls, this_val, static_cast<int64_t>(slab.size()), slab.data());
}

// --- Iterator protocol runtime --------------------------------------------
//
// Kotlin-style protocol: iterator Objects expose three 0-arg closures —
// `iter` (returns self), `has_next` (Bool), `next` (Any). The JIT
// drives for-in and the terminal/lazy methods through C++ runtime
// helpers so each call site emits one factory / terminal call rather
// than a full closure body. State for built-in factories lives in
// JitCells attached to both trampolines; the last two cells are a
// shared lookahead pair (see `_iter_wrap_new`) that lets `has_next()`
// peek and `next()` consume without re-pulling from `fast_next_fn`.
//
// Conventions:
//   - `_iter_advance_raw(has_next_cls, next_cls, iter, out_tag, out_data)`
//     pulls one value via the cached lookahead when present, otherwise
//     `fast_next_fn` (or the user's has_next/next pair).
//   - Terminal methods consume the iterator (don't release it; caller
//     holds the +1 and will release after the terminal returns).
//   - Lazy wrapper factories transfer upstream's +1 into a capture
//     cell so the wrapper owns it for its lifetime.

// Forward declared — defined later alongside the array higher-order
// runtime helpers.
inline JitClosure* _culebra_expect_callback(int8_t fn_tag, int64_t fn_data,
                                            size_t expected_arity,
                                            const char* method_name,
                                            int64_t line, int64_t col);
extern "C" CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray*
culebra_runtime_object_keys(JitObject* obj);

// Generic "iter returns self" — used by every wrapper we build.
inline JitValue _iter_self_iter_fn(JitClosure*, JitValue this_val,
                                   int64_t, JitValue*) {
  culebra_runtime_value_retain(this_val.tag, this_val.data);
  return this_val;
}

// Look up a 0-arg method closure on an iterator Object. Returns nullptr
// if the receiver isn't a TAG_OBJECT or doesn't expose `method` as a
// Function. Used once at walk entry so each step skips the per-iteration
// std::map::find.
inline JitClosure* _iter_method_closure(JitValue iter_val,
                                        const char* method) {
  if (iter_val.tag != TAG_OBJECT) return nullptr;
  auto* iter_obj = reinterpret_cast<JitObject*>(iter_val.data);
  auto idx = iter_obj->find_slot(method);
  if (idx == static_cast<size_t>(-1)) return nullptr;
  const auto& entry = iter_obj->slots[idx];
  if (entry.value.tag != TAG_FUNC) return nullptr;
  return reinterpret_cast<JitClosure*>(entry.value.data);
}

inline JitClosure* _iter_has_next_closure(JitValue iter_val) {
  return _iter_method_closure(iter_val, "has_next");
}

inline JitClosure* _iter_next_closure(JitValue iter_val) {
  return _iter_method_closure(iter_val, "next");
}

// Pre-resolve an upstream's has_next / next closures into a pair of
// TAG_LONG cells the lazy factories below thread into their captures.
// Folds the 4-line "cell_new for has_next, cell_new for next" pattern
// every map/filter/take/etc. factory used to repeat verbatim — and
// kept their argument order in sync so chain/zip's index shuffle
// doesn't drift again.
struct IterClosureCells {
  JitCell* has_next;
  JitCell* next;
};
inline IterClosureCells _iter_cache_closure_cells(JitValue iv) {
  return {
      culebra_runtime_cell_new(
          TAG_LONG, reinterpret_cast<int64_t>(_iter_has_next_closure(iv))),
      culebra_runtime_cell_new(
          TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure(iv))),
  };
}

// Lookahead state values stored in the state cell appended by
// `_iter_wrap_fast`. The cell carries TAG_LONG with these payloads.
enum : int64_t {
  _ITER_LA_EMPTY   = 0,   // no cached value; pull on demand
  _ITER_LA_FILLED  = 1,   // value cell holds a +1 cached lookahead
  _ITER_LA_DRAINED = 2,   // upstream returned done; has_next stays false
};

// Pull the next value via out-params. For iterators built by
// `_iter_wrap_fast` (i.e. those exposing `fast_next_fn`), consults the
// lookahead state cells appended by that factory so a prior
// `has_next()` peek is consumed transparently. For user-defined
// iterators (no fast_next_fn) calls `has_next()` then `next()` on the
// user-visible closures.
inline bool _iter_advance_raw(JitClosure* has_next_cls, JitClosure* next_cls,
                              JitValue iter_val,
                              int8_t* out_tag, int64_t* out_data) {
  if (iter_val.tag == TAG_OBJECT) {
    auto* iter_obj = reinterpret_cast<JitObject*>(iter_val.data);
    if (iter_obj->fast_next_fn) {
      size_t n = next_cls->n_captures;
      JitCell* state_cell = next_cls->captures[n - 2];
      JitCell* value_cell = next_cls->captures[n - 1];
      if (state_cell->value.data == _ITER_LA_FILLED) {
        // Consume the cached peek; ownership of value_cell's +1
        // transfers to the caller.
        *out_tag = value_cell->value.tag;
        *out_data = value_cell->value.data;
        state_cell->value = {TAG_LONG, _ITER_LA_EMPTY};
        value_cell->value = {TAG_NIL, 0};
        return true;
      }
      if (state_cell->value.data == _ITER_LA_DRAINED) return false;
      bool done;
      iter_obj->fast_next_fn(next_cls, iter_val, &done, out_tag, out_data);
      if (done) {
        state_cell->value = {TAG_LONG, _ITER_LA_DRAINED};
        return false;
      }
      return true;
    }
  }
  // Slow path: user-built iterator. Drive its has_next() / next()
  // closures directly; the user owns any caching they need.
  culebra_runtime_value_retain(iter_val.tag, iter_val.data);
  auto hn = reinterpret_cast<JitFn>(has_next_cls->fn_ptr)(
      has_next_cls, iter_val, 0, nullptr);
  bool has = (hn.tag == TAG_BOOL && hn.data != 0);
  _culebra_value_release_impl(hn.tag, hn.data);
  if (!has) return false;
  culebra_runtime_value_retain(iter_val.tag, iter_val.data);
  auto v = reinterpret_cast<JitFn>(next_cls->fn_ptr)(
      next_cls, iter_val, 0, nullptr);
  *out_tag = v.tag;
  *out_data = v.data;
  return true;
}

// Pull the next value from an iterator using pre-cached `has_next` +
// `next` closures. Returns true with `out_v` holding a +1-retained
// Value on success; false on done. Uses the raw fast path when
// available (see `_iter_advance_raw`).
inline bool _iter_pull(JitClosure* has_next_cls, JitClosure* next_cls,
                       JitValue iter_val, JitValue& out_v) {
  return _iter_advance_raw(has_next_cls, next_cls, iter_val,
                            &out_v.tag, &out_v.data);
}

// Forward decl: `culebra_runtime_cell_retain` is defined further down
// alongside the other refcount entries (one big TU; the iterator
// factories above need this before that definition is parsed).
extern "C" {
CULEBRA_RT_KEEP void culebra_runtime_cell_retain(JitCell* c);
}

// Build a wrapper iterator Object exposing the Kotlin-style
// `Iterator { has_next() -> Bool, next() -> Any }` + `Iterable.iter()`
// protocol. Both has_next / next closures share the SAME captures
// slab (built once from `captures` + 2 trailing lookahead cells so
// `_iter_advance_raw` and the user-visible trampolines can peek/cache
// transparently).
//
// Captures layout: [user's captures..., state_cell, value_cell].
// state_cell.value.data ∈ { _ITER_LA_EMPTY, _ITER_LA_FILLED, _ITER_LA_DRAINED }.
// value_cell holds a +1-owned lookahead while state == FILLED.
inline JitObject* _iter_wrap_new(
    JitValue (*has_next_fn)(JitClosure*, JitValue, int64_t, JitValue*),
    JitValue (*next_fn)(JitClosure*, JitValue, int64_t, JitValue*),
    std::initializer_list<JitCell*> captures) {
  auto* state_cell = culebra_runtime_cell_new(TAG_LONG, _ITER_LA_EMPTY);
  auto* value_cell = culebra_runtime_cell_new(TAG_NIL, 0);
  size_t total = captures.size() + 2;
  auto write_captures = [&](JitClosure* cls) {
    size_t i = 0;
    for (auto* c : captures) {
      culebra_runtime_cell_retain(c);
      cls->captures[i++] = c;
    }
    culebra_runtime_cell_retain(state_cell);
    cls->captures[i++] = state_cell;
    culebra_runtime_cell_retain(value_cell);
    cls->captures[i++] = value_cell;
  };
  auto* iter_cls = culebra_runtime_closure_new(
      reinterpret_cast<void*>(&_iter_self_iter_fn), 0, 0);
  auto* has_next_cls = culebra_runtime_closure_new(
      reinterpret_cast<void*>(has_next_fn), total, 0);
  write_captures(has_next_cls);
  auto* next_cls = culebra_runtime_closure_new(
      reinterpret_cast<void*>(next_fn), total, 0);
  write_captures(next_cls);
  // Caller transferred +1 on each user cell — release the originals
  // since we now hold one retain per cell per closure (above).
  for (auto* c : captures) _culebra_cell_release(c);
  _culebra_cell_release(state_cell);
  _culebra_cell_release(value_cell);
  auto* obj = culebra_runtime_object_new();
  culebra_runtime_object_set(obj, "iter", /*mut*/ false, GC_TAG_FUNC,
                             reinterpret_cast<int64_t>(iter_cls), 0, 0);
  culebra_runtime_object_set(obj, "has_next", /*mut*/ false, GC_TAG_FUNC,
                             reinterpret_cast<int64_t>(has_next_cls), 0, 0);
  culebra_runtime_object_set(obj, "next", /*mut*/ false, GC_TAG_FUNC,
                             reinterpret_cast<int64_t>(next_cls), 0, 0);
  return obj;
}

}  // extern "C" (close briefly for the iterator-wrapper templates)

// has_next() trampoline. Peeks one value via FastFn (caching in the
// state cells) so a subsequent `next()` can consume the cached value
// without re-invoking FastFn. Idempotent on repeat calls.
template <JitIterFastFn FastFn>
inline JitValue _iter_trampoline_has_next_fn(JitClosure* cls, JitValue iv,
                                              int64_t, JitValue*) {
  size_t n = cls->n_captures;
  JitCell* state_cell = cls->captures[n - 2];
  JitCell* value_cell = cls->captures[n - 1];
  if (state_cell->value.data == _ITER_LA_FILLED) return {TAG_BOOL, 1};
  if (state_cell->value.data == _ITER_LA_DRAINED) return {TAG_BOOL, 0};
  bool done;
  int8_t tag;
  int64_t data;
  FastFn(cls, iv, &done, &tag, &data);
  if (done) {
    state_cell->value = {TAG_LONG, _ITER_LA_DRAINED};
    return {TAG_BOOL, 0};
  }
  state_cell->value = {TAG_LONG, _ITER_LA_FILLED};
  value_cell->value = {tag, data};   // +1 transfers into the cell
  return {TAG_BOOL, 1};
}

// next() trampoline. Returns the cached lookahead when present (cheap
// path after has_next()), otherwise pulls a fresh value via FastFn.
// Calling next() past end yields nil; the contract advises pairing
// with has_next() to avoid that case.
template <JitIterFastFn FastFn>
inline JitValue _iter_trampoline_next_fn(JitClosure* cls, JitValue iv,
                                          int64_t, JitValue*) {
  size_t n = cls->n_captures;
  JitCell* state_cell = cls->captures[n - 2];
  JitCell* value_cell = cls->captures[n - 1];
  if (state_cell->value.data == _ITER_LA_FILLED) {
    auto v = value_cell->value;
    state_cell->value = {TAG_LONG, _ITER_LA_EMPTY};
    value_cell->value = {TAG_NIL, 0};
    return v;  // +1 transfers to caller
  }
  if (state_cell->value.data == _ITER_LA_DRAINED) return {TAG_NIL, 0};
  bool done;
  int8_t tag;
  int64_t data;
  FastFn(cls, iv, &done, &tag, &data);
  if (done) {
    state_cell->value = {TAG_LONG, _ITER_LA_DRAINED};
    return {TAG_NIL, 0};
  }
  return {tag, data};
}

// Build a wrapper Object whose user-visible has_next / next trampoline
// to `FastFn` (shared lookahead cells) and whose `fast_next_fn` slot
// exposes `FastFn` directly so `_iter_advance_raw` can take the fast
// path while still respecting any cached peek.
template <JitIterFastFn FastFn>
inline JitObject* _iter_wrap_fast(std::initializer_list<JitCell*> captures) {
  auto* obj = _iter_wrap_new(&_iter_trampoline_has_next_fn<FastFn>,
                              &_iter_trampoline_next_fn<FastFn>, captures);
  obj->fast_next_fn = FastFn;
  return obj;
}

extern "C" {

// --- Terminal iterator methods --------------------------------------------

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_iter_collect(
    int8_t it, int64_t id) {
  auto* out = culebra_runtime_array_new();
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    culebra_runtime_array_push(out, v.tag, v.data);  // consumes +1
  }
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_count(
    int8_t it, int64_t id) {
  int64_t n = 0;
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    _culebra_value_release_impl(v.tag, v.data);
    n++;
  }
  return n;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_iter_for_each(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(ft, fd, 1, "for_each", line, col);
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    auto r = _culebra_invoke1(fn, v);
    _culebra_value_release_impl(r.tag, r.data);
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_iter_reduce(
    int8_t it, int64_t id, int8_t init_tag, int64_t init_data,
    int8_t ft, int64_t fd, int64_t line, int64_t col, int8_t* out_tag,
    int64_t* out_data) {
  auto* fn = _culebra_expect_callback(ft, fd, 2, "reduce", line, col);
  JitValue acc = {init_tag, init_data};
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    acc = _culebra_invoke2(fn, acc, v);
  }
  *out_tag = acc.tag;
  *out_data = acc.data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_iter_find(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col,
    int8_t* out_tag, int64_t* out_data) {
  auto* fn = _culebra_expect_callback(ft, fd, 1, "find", line, col);
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    culebra_runtime_value_retain(v.tag, v.data);  // an extra +1 for the call
    auto r = _culebra_invoke1(fn, v);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) {
      *out_tag = v.tag;
      *out_data = v.data;  // out-param takes v's original +1
      return;
    }
    _culebra_value_release_impl(v.tag, v.data);
  }
  *out_tag = 0;
  *out_data = 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_any(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(ft, fd, 1, "any", line, col);
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    auto r = _culebra_invoke1(fn, v);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) return 1;
  }
  return 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_all(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(ft, fd, 1, "all", line, col);
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    auto r = _culebra_invoke1(fn, v);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (!keep) return 0;
  }
  return 1;
}

// Sum/product/min/max share the same iterate-and-accumulate shape.
// Non-Long elements raise `type error at L:C.`, matching the interp's
// `Value::to_long`. `min` / `max` on an empty input also throw, since
// there's no natural identity for them.

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_sum(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  int64_t acc = 0;
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    if (v.tag != TAG_LONG) {
      _culebra_value_release_impl(v.tag, v.data);
      culebra::throw_type_error_at(line, col);
    }
    acc += v.data;
  }
  return acc;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_product(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  int64_t acc = 1;
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    if (v.tag != TAG_LONG) {
      _culebra_value_release_impl(v.tag, v.data);
      culebra::throw_type_error_at(line, col);
    }
    acc *= v.data;
  }
  return acc;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_min(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  if (!_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    throw culebra::CulebraError("ValueError", std::format(
        "min of empty Iterator at {}:{}.", line, col), line, col);
  }
  if (v.tag != TAG_LONG) {
    _culebra_value_release_impl(v.tag, v.data);
    culebra::throw_type_error_at(line, col);
  }
  int64_t best = v.data;
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    if (v.tag != TAG_LONG) {
      _culebra_value_release_impl(v.tag, v.data);
      culebra::throw_type_error_at(line, col);
    }
    if (v.data < best) best = v.data;
  }
  return best;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_max(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  if (!_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    throw culebra::CulebraError("ValueError", std::format(
        "max of empty Iterator at {}:{}.", line, col), line, col);
  }
  if (v.tag != TAG_LONG) {
    _culebra_value_release_impl(v.tag, v.data);
    culebra::throw_type_error_at(line, col);
  }
  int64_t best = v.data;
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    if (v.tag != TAG_LONG) {
      _culebra_value_release_impl(v.tag, v.data);
      culebra::throw_type_error_at(line, col);
    }
    if (v.data > best) best = v.data;
  }
  return best;
}

// --- Lazy iterator wrappers: each factory returns a new iterator Object.
// Captures are stored in cells attached to the wrapper's `next` closure.
// Cells transfer their +1 to the closure; callers should not retain
// after handing a value to a factory.

// map: captures upstream + fn. Fast form feeds cascading raw-advance
// through chained wrappers so a deep map/filter/take stack pays zero
// step-object allocations per iteration. Upstream's `next` closure is
// cached at factory time in captures[2] to drop the per-step map::find.
inline void _iter_map_fast_fn(JitClosure* cls, JitValue, bool* done,
                              int8_t* out_tag, int64_t* out_data) {
  auto upstream = cls->captures[0]->value;
  auto fnv = cls->captures[1]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  int8_t tag;
  int64_t data;
  if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
    *done = true;
    return;
  }
  auto* fn_cls = reinterpret_cast<JitClosure*>(fnv.data);
  auto mapped = _culebra_invoke1(fn_cls, {tag, data});
  *done = false;
  *out_tag = mapped.tag;
  *out_data = mapped.data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_map(
    int8_t it, int64_t id, int8_t ft, int64_t fd) {
  culebra_runtime_value_retain(it, id);
  culebra_runtime_value_retain(ft, fd);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* f = culebra_runtime_cell_new(ft, fd);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_map_fast_fn>(
      {up, f, up_has_next_cell, up_next_cell});
}

// filter: captures upstream + predicate + cached upstream next.
inline void _iter_filter_fast_fn(JitClosure* cls, JitValue, bool* done,
                                 int8_t* out_tag, int64_t* out_data) {
  auto upstream = cls->captures[0]->value;
  auto fnv = cls->captures[1]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* fn_cls = reinterpret_cast<JitClosure*>(fnv.data);
  for (;;) {
    int8_t tag;
    int64_t data;
    if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
      *done = true;
      return;
    }
    JitValue v = {tag, data};
    culebra_runtime_value_retain(v.tag, v.data);  // for callback
    auto r = _culebra_invoke1(fn_cls, v);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) {
      *done = false;
      *out_tag = v.tag;
      *out_data = v.data;
      return;
    }
    _culebra_value_release_impl(v.tag, v.data);
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_filter(
    int8_t it, int64_t id, int8_t ft, int64_t fd) {
  culebra_runtime_value_retain(it, id);
  culebra_runtime_value_retain(ft, fd);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* f = culebra_runtime_cell_new(ft, fd);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_filter_fast_fn>(
      {up, f, up_has_next_cell, up_next_cell});
}

// take: captures upstream + remaining (Long cell, decremented each step)
//       + cached upstream next closure.
inline void _iter_take_fast_fn(JitClosure* cls, JitValue, bool* done,
                               int8_t* out_tag, int64_t* out_data) {
  auto* rem_cell = cls->captures[1];
  if (rem_cell->value.data <= 0) {
    *done = true;
    return;
  }
  auto upstream = cls->captures[0]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  int8_t tag;
  int64_t data;
  if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
    *done = true;
    return;
  }
  rem_cell->value.data--;
  *done = false;
  *out_tag = tag;
  *out_data = data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_take(
    int8_t it, int64_t id, int64_t n) {
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* rem = culebra_runtime_cell_new(TAG_LONG, n);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_take_fast_fn>(
      {up, rem, up_has_next_cell, up_next_cell});
}

// skip: captures upstream + remaining-to-skip + cached upstream next.
inline void _iter_skip_fast_fn(JitClosure* cls, JitValue, bool* done,
                               int8_t* out_tag, int64_t* out_data) {
  auto* rem_cell = cls->captures[1];
  auto upstream = cls->captures[0]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  while (rem_cell->value.data > 0) {
    int8_t t;
    int64_t d;
    if (!_iter_advance_raw(up_has_next, up_next, upstream, &t, &d)) {
      *done = true;
      return;
    }
    _culebra_value_release_impl(t, d);
    rem_cell->value.data--;
  }
  if (!_iter_advance_raw(up_has_next, up_next, upstream, out_tag, out_data)) {
    *done = true;
    return;
  }
  *done = false;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_skip(
    int8_t it, int64_t id, int64_t n) {
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* rem = culebra_runtime_cell_new(TAG_LONG, n);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_skip_fast_fn>(
      {up, rem, up_has_next_cell, up_next_cell});
}

// take_while: captures upstream + predicate + stopped flag + cached upstream
// next.
inline void _iter_take_while_fast_fn(JitClosure* cls, JitValue, bool* done,
                                     int8_t* out_tag, int64_t* out_data) {
  auto* stopped_cell = cls->captures[2];
  if (stopped_cell->value.data) {
    *done = true;
    return;
  }
  auto upstream = cls->captures[0]->value;
  auto pv = cls->captures[1]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[4]->value.data);
  auto* fn_cls = reinterpret_cast<JitClosure*>(pv.data);
  int8_t tag;
  int64_t data;
  if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
    *done = true;
    return;
  }
  JitValue v = {tag, data};
  culebra_runtime_value_retain(v.tag, v.data);
  auto r = _culebra_invoke1(fn_cls, v);
  bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
  _culebra_value_release_impl(r.tag, r.data);
  if (!keep) {
    stopped_cell->value.data = 1;
    _culebra_value_release_impl(v.tag, v.data);
    *done = true;
    return;
  }
  *done = false;
  *out_tag = v.tag;
  *out_data = v.data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_take_while(
    int8_t it, int64_t id, int8_t ft, int64_t fd) {
  culebra_runtime_value_retain(it, id);
  culebra_runtime_value_retain(ft, fd);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* p = culebra_runtime_cell_new(ft, fd);
  auto* stopped = culebra_runtime_cell_new(TAG_BOOL, 0);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_take_while_fast_fn>(
      {up, p, stopped, up_has_next_cell, up_next_cell});
}

// enumerate: captures upstream + index + cached upstream next.
inline void _iter_enumerate_fast_fn(JitClosure* cls, JitValue, bool* done,
                                    int8_t* out_tag, int64_t* out_data) {
  auto upstream = cls->captures[0]->value;
  auto* idx_cell = cls->captures[1];
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  int8_t tag;
  int64_t data;
  if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
    *done = true;
    return;
  }
  auto* pair = culebra_runtime_object_new();
  culebra_runtime_object_set(pair, "index", false, TAG_LONG,
                             idx_cell->value.data, 0, 0);
  culebra_runtime_object_set(pair, "value", false, tag, data, 0, 0);
  idx_cell->value.data++;
  *done = false;
  *out_tag = TAG_OBJECT;
  *out_data = reinterpret_cast<int64_t>(pair);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_enumerate(
    int8_t it, int64_t id) {
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* idx = culebra_runtime_cell_new(TAG_LONG, 0);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_enumerate_fast_fn>(
      {up, idx, up_has_next_cell, up_next_cell});
}

// chain: captures iter1 + iter2 + phase + has_next/next pair for each.
inline void _iter_chain_fast_fn(JitClosure* cls, JitValue, bool* done,
                                int8_t* out_tag, int64_t* out_data) {
  auto* phase_cell = cls->captures[2];
  auto* h1 = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* n1 = reinterpret_cast<JitClosure*>(cls->captures[4]->value.data);
  auto* h2 = reinterpret_cast<JitClosure*>(cls->captures[5]->value.data);
  auto* n2 = reinterpret_cast<JitClosure*>(cls->captures[6]->value.data);
  if (phase_cell->value.data == 0) {
    auto up1 = cls->captures[0]->value;
    if (_iter_advance_raw(h1, n1, up1, out_tag, out_data)) {
      *done = false;
      return;
    }
    phase_cell->value.data = 1;
  }
  auto up2 = cls->captures[1]->value;
  if (_iter_advance_raw(h2, n2, up2, out_tag, out_data)) {
    *done = false;
    return;
  }
  *done = true;
}

inline JitObject* _iter_from_array_obj(int8_t at, int64_t ad);

// Coerce any iterable (Array / Object-with-iter / already-iterator) into an
// iterator Object Value. Returns a fresh +1 (caller owns the result).
// Throws `type error at line:col.` on non-iterable input.
inline JitValue _iter_coerce_iterable(int8_t t, int64_t d, int64_t line,
                                      int64_t col) {
  if (t == TAG_ARRAY || t == TAG_TUPLE) {
    auto* wrapped =
        _iter_from_array_obj(t, d);
    return {TAG_OBJECT, reinterpret_cast<int64_t>(wrapped)};
  }
  if (t == TAG_OBJECT) {
    auto* o = reinterpret_cast<JitObject*>(d);
    auto idx = o->find_slot("iter");
    if (idx != static_cast<size_t>(-1) &&
        o->slots[idx].value.tag == TAG_FUNC) {
      auto iv = o->slots[idx].value;
      auto* iv_cls = reinterpret_cast<JitClosure*>(iv.data);
      culebra_runtime_value_retain(t, d);
      return reinterpret_cast<JitFn>(iv_cls->fn_ptr)(iv_cls, {t, d}, 0,
                                                     nullptr);
    }
  }
  throw culebra::CulebraError("TypeError", std::format(
      "type error: target is not iterable at {}:{}.", line, col), line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_chain(
    int8_t it1, int64_t id1, int8_t it2, int64_t id2, int64_t line,
    int64_t col) {
  // Coerce both inputs so raw Arrays can be chained without the caller
  // having to call `.iter()` first. Each coerce call returns a fresh +1.
  auto iv1 = _iter_coerce_iterable(it1, id1, line, col);
  auto iv2 = _iter_coerce_iterable(it2, id2, line, col);
  auto* u1 = culebra_runtime_cell_new(iv1.tag, iv1.data);
  auto* u2 = culebra_runtime_cell_new(iv2.tag, iv2.data);
  auto* phase = culebra_runtime_cell_new(TAG_LONG, 0);
  auto c1 = _iter_cache_closure_cells(iv1);
  auto c2 = _iter_cache_closure_cells(iv2);
  return _iter_wrap_fast<&_iter_chain_fast_fn>(
      {u1, u2, phase, c1.has_next, c1.next, c2.has_next, c2.next});
}

// zip: captures iter1 + iter2 + has_next/next pair for each.
inline void _iter_zip_fast_fn(JitClosure* cls, JitValue, bool* done,
                              int8_t* out_tag, int64_t* out_data) {
  auto up1 = cls->captures[0]->value;
  auto up2 = cls->captures[1]->value;
  auto* h1 = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* n1 = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* h2 = reinterpret_cast<JitClosure*>(cls->captures[4]->value.data);
  auto* n2 = reinterpret_cast<JitClosure*>(cls->captures[5]->value.data);
  int8_t t1, t2;
  int64_t d1, d2;
  if (!_iter_advance_raw(h1, n1, up1, &t1, &d1)) {
    *done = true;
    return;
  }
  if (!_iter_advance_raw(h2, n2, up2, &t2, &d2)) {
    _culebra_value_release_impl(t1, d1);
    *done = true;
    return;
  }
  auto* pair = culebra_runtime_object_new();
  culebra_runtime_object_set(pair, "first", false, t1, d1, 0, 0);
  culebra_runtime_object_set(pair, "second", false, t2, d2, 0, 0);
  *done = false;
  *out_tag = TAG_OBJECT;
  *out_data = reinterpret_cast<int64_t>(pair);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_zip(
    int8_t it1, int64_t id1, int8_t it2, int64_t id2, int64_t line,
    int64_t col) {
  auto iv1 = _iter_coerce_iterable(it1, id1, line, col);
  auto iv2 = _iter_coerce_iterable(it2, id2, line, col);
  auto* u1 = culebra_runtime_cell_new(iv1.tag, iv1.data);
  auto* u2 = culebra_runtime_cell_new(iv2.tag, iv2.data);
  auto c1 = _iter_cache_closure_cells(iv1);
  auto c2 = _iter_cache_closure_cells(iv2);
  return _iter_wrap_fast<&_iter_zip_fast_fn>(
      {u1, u2, c1.has_next, c1.next, c2.has_next, c2.next});
}

// --- String iterator wrappers --------------------------------------------
// String.code_points() and .graphemes() are implemented as iterator
// factories that wrap the underlying UTF-8 buffer. Each walks the
// String lazily — one decode per next() step — mirroring the interp
// behaviour in `string_builtins()`.

inline void _iter_code_points_fast_fn(JitClosure* cls, JitValue, bool* done,
                                      int8_t* out_tag, int64_t* out_data) {
  auto buf_cell = cls->captures[0];
  auto off_cell = cls->captures[1];
  auto len_cell = cls->captures[2];
  const char* s = reinterpret_cast<const char*>(buf_cell->value.data);
  int64_t off = off_cell->value.data;
  int64_t len = len_cell->value.data;
  if (off >= len) {
    *done = true;
    return;
  }
  char32_t cp;
  size_t bytes;
  if (!unicode::utf8::decode_codepoint(s + off, len - off, bytes, cp)) {
    cp = static_cast<unsigned char>(s[off]);
    bytes = 1;
  }
  off_cell->value.data = off + static_cast<int64_t>(bytes);
  *done = false;
  *out_tag = TAG_LONG;
  *out_data = static_cast<int64_t>(cp);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_str_code_points(
    const char* s) {
  // Strings are not refcounted in JIT; the pointer stays valid as long
  // as the source String stays rooted somewhere (usually via the
  // caller's own slot). Lifetime is documented in §16.
  auto* buf_cell = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(s));
  auto* off_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* len_cell = culebra_runtime_cell_new(
      TAG_LONG, static_cast<int64_t>(std::strlen(s)));
  return _iter_wrap_fast<&_iter_code_points_fast_fn>(
      {buf_cell, off_cell, len_cell});
}

// graphemes: eagerly materialize all cluster boundaries into a JitArray
// at factory time, then reuse the generic Array walker. This avoids a
// u32string-shaped leak (no cell tag currently owns `delete` of a
// non-trivial C++ type) and keeps the runtime surface minimal. The
// trade-off is a single O(n) decode up front instead of streaming —
// acceptable because user code that wants streaming usually prefers
// `.code_points()` anyway.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_str_graphemes(
    const char* s) {
  std::u32string u32;
  size_t buf_size = std::strlen(s);
  unicode::utf8::decode(s, buf_size, u32);
  auto* arr = culebra_runtime_array_new();
  size_t cp_off = 0;
  while (cp_off < u32.size()) {
    size_t gl = unicode::grapheme_length(u32.data() + cp_off,
                                         u32.size() - cp_off);
    if (gl == 0) gl = 1;
    std::string out;
    unicode::utf8::encode(u32.data() + cp_off, gl, out);
    auto* heap = _culebra_heap_str(out);
    culebra_runtime_array_push(arr, TAG_STRING,
                               reinterpret_cast<int64_t>(heap));
    cp_off += gl;
  }
  return _iter_from_array_obj(TAG_ARRAY, reinterpret_cast<int64_t>(arr));
}

// Wrap a JitArray as a one-shot iterator Object. Used to drive
// flat_map callbacks that return Arrays.
inline void _iter_from_array_fast_fn(JitClosure* cls, JitValue, bool* done,
                                     int8_t* out_tag, int64_t* out_data) {
  auto arr_cell = cls->captures[0];
  auto idx_cell = cls->captures[1];
  auto* arr = reinterpret_cast<JitArray*>(arr_cell->value.data);
  int64_t idx = idx_cell->value.data;
  if (static_cast<size_t>(idx) >= arr->size) {
    *done = true;
    return;
  }
  auto v = arr->items[idx];
  culebra_runtime_value_retain(v.tag, v.data);
  idx_cell->value.data = idx + 1;
  *done = false;
  *out_tag = v.tag;
  *out_data = v.data;
}

inline JitObject* _iter_from_array_obj(int8_t at, int64_t ad) {
  culebra_runtime_value_retain(at, ad);
  auto* arr_cell = culebra_runtime_cell_new(at, ad);
  auto* idx_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  return _iter_wrap_fast<&_iter_from_array_fast_fn>({arr_cell, idx_cell});
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_array_iter(
    JitArray* arr) {
  return _iter_from_array_obj(TAG_ARRAY, reinterpret_cast<int64_t>(arr));
}

// Object.iter() fast fn. Captures: [obj_cell, keys_array_cell, idx_cell,
// snapshot_cell]. Snapshot is obj->mut_count at iter-init; a per-step
// mismatch raises a RuntimeError (Python dict-iter semantics).
inline void _iter_from_object_fast_fn(JitClosure* cls, JitValue,
                                      bool* done, int8_t* out_tag,
                                      int64_t* out_data) {
  auto obj_cell = cls->captures[0];
  auto arr_cell = cls->captures[1];
  auto idx_cell = cls->captures[2];
  auto snap_cell = cls->captures[3];
  auto* obj = reinterpret_cast<JitObject*>(obj_cell->value.data);
  if (obj->mut_count != snap_cell->value.data) {
    culebra_runtime_throw_error(
        "RuntimeError", "Object changed size during iteration", 0, 0);
    *done = true;
    return;
  }
  auto* arr = reinterpret_cast<JitArray*>(arr_cell->value.data);
  int64_t idx = idx_cell->value.data;
  if (static_cast<size_t>(idx) >= arr->size) {
    *done = true;
    return;
  }
  auto v = arr->items[idx];
  culebra_runtime_value_retain(v.tag, v.data);
  idx_cell->value.data = idx + 1;
  *done = false;
  *out_tag = v.tag;
  *out_data = v.data;
}

// `obj.iter()` dispatcher used by JIT — prefers a user-defined `iter`
// method (so an Object that already exposes the Iterator protocol
// returns itself / its underlying state) and only falls back to the
// key iterator from `ObjectValue::builtins()` for plain literals
// without a user iter. Mirrors interp's `_get_iterator`.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject*
culebra_runtime_object_iter_dispatch(JitObject* obj);

// Object.iter(): yield keys in insertion order as Strings. Structural
// mutation of the underlying Object (add/delete) during iteration
// raises; value updates on existing keys are allowed.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_object_iter(
    JitObject* obj) {
  auto* keys = culebra_runtime_object_keys(obj);
  culebra_runtime_value_retain(TAG_OBJECT, reinterpret_cast<int64_t>(obj));
  auto* obj_cell = culebra_runtime_cell_new(
      TAG_OBJECT, reinterpret_cast<int64_t>(obj));
  auto* arr_cell = culebra_runtime_cell_new(
      TAG_ARRAY, reinterpret_cast<int64_t>(keys));
  auto* idx_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* snap_cell = culebra_runtime_cell_new(TAG_LONG, obj->mut_count);
  return _iter_wrap_fast<&_iter_from_object_fast_fn>(
      {obj_cell, arr_cell, idx_cell, snap_cell});
}

// Dispatch helper: prefer the user `iter` slot when present so an
// Object that already implements Iterator returns the right iterator
// (range/map/etc. all hit this), and fall back to the key iterator for
// plain `{...}` literals via `ObjectValue::builtins()`.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject*
culebra_runtime_object_iter_dispatch(JitObject* obj) {
  auto idx = obj->find_slot("iter");
  if (idx != static_cast<size_t>(-1) &&
      obj->slots[idx].value.tag == TAG_FUNC) {
    auto* cls = reinterpret_cast<JitClosure*>(obj->slots[idx].value.data);
    auto self = JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(obj)};
    culebra_runtime_value_retain(self.tag, self.data);
    auto r = reinterpret_cast<JitFn>(cls->fn_ptr)(cls, self, 0, nullptr);
    // The user iter() returns an Object (+1) — caller takes that retain.
    return reinterpret_cast<JitObject*>(r.data);
  }
  return culebra_runtime_object_iter(obj);
}

// flat_map captures: upstream, fn, current-inner-iter (nullable),
// has_next/next pair for upstream, has_next/next pair for the current
// inner (refreshed per-element when fn() returns a new iterable), and
// a packed (line << 32 | col) cell for error reporting when the
// callback yields a non-iterable.
inline void _iter_flat_map_fast_fn(JitClosure* cls, JitValue, bool* done,
                                   int8_t* out_tag, int64_t* out_data) {
  auto upstream = cls->captures[0]->value;
  auto fnv = cls->captures[1]->value;
  auto* fn_cls = reinterpret_cast<JitClosure*>(fnv.data);
  auto* inner_cell = cls->captures[2];
  auto* up_has_next =
      reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* up_next =
      reinterpret_cast<JitClosure*>(cls->captures[4]->value.data);
  auto* inner_has_next_cell = cls->captures[5];
  auto* inner_next_cell = cls->captures[6];
  int64_t packed = cls->captures[7]->value.data;
  int64_t line = packed >> 32;
  int64_t col = packed & 0xFFFFFFFF;
  for (;;) {
    auto inner_v = inner_cell->value;
    if (inner_v.tag == TAG_OBJECT && inner_v.data != 0) {
      auto* inner_has_next =
          reinterpret_cast<JitClosure*>(inner_has_next_cell->value.data);
      auto* inner_next =
          reinterpret_cast<JitClosure*>(inner_next_cell->value.data);
      if (_iter_advance_raw(inner_has_next, inner_next, inner_v,
                             out_tag, out_data)) {
        *done = false;
        return;
      }
      _culebra_value_release_impl(inner_v.tag, inner_v.data);
      inner_cell->value = {0, 0};
      inner_has_next_cell->value = {TAG_LONG, 0};
      inner_next_cell->value = {TAG_LONG, 0};
    }
    int8_t tag;
    int64_t data;
    if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
      *done = true;
      return;
    }
    auto mapped = _culebra_invoke1(fn_cls, {tag, data});
    _culebra_value_release_impl(tag, data);
    auto iv = _iter_coerce_iterable(mapped.tag, mapped.data, line, col);
    _culebra_value_release_impl(mapped.tag, mapped.data);
    inner_cell->value = iv;
    inner_has_next_cell->value = {
        TAG_LONG, reinterpret_cast<int64_t>(_iter_has_next_closure(iv))};
    inner_next_cell->value = {
        TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure(iv))};
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_flat_map(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line,
    int64_t col) {
  culebra_runtime_value_retain(it, id);
  culebra_runtime_value_retain(ft, fd);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* f = culebra_runtime_cell_new(ft, fd);
  auto* inner = culebra_runtime_cell_new(TAG_NIL, 0);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* inner_has_next = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* inner_next = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* loc = culebra_runtime_cell_new(
      TAG_LONG, (line << 32) | (col & 0xFFFFFFFF));
  return _iter_wrap_fast<&_iter_flat_map_fast_fn>(
      {up, f, inner, up_cells.has_next, up_cells.next,
       inner_has_next, inner_next, loc});
}

inline void _math_range_fast_fn(JitClosure* cls, JitValue, bool* done,
                                int8_t* out_tag, int64_t* out_data) {
  auto* current_cell = cls->captures[0];
  auto* end_cell = cls->captures[1];
  auto* step_cell = cls->captures[2];
  int64_t current = current_cell->value.data;
  int64_t end = end_cell->value.data;
  int64_t step = step_cell->value.data;
  bool finished = step > 0 ? current >= end : current <= end;
  if (finished) {
    *done = true;
    return;
  }
  *done = false;
  *out_tag = TAG_LONG;
  *out_data = current;
  current_cell->value.data = current + step;
}

// Entry point called from for-in codegen (`rt::iter_advance`): returns
// 1 on a value step with `*out_tag`/`*out_data` holding a +1 Value, 0
// on done. `next_cls` is the `iter.next` closure resolved once before
// the loop; forwarded as the `this` capture slab on the fast path.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_advance(
    JitClosure* has_next_cls, JitClosure* next_cls,
    int8_t it, int64_t id, int8_t* out_tag, int64_t* out_data) {
  return _iter_advance_raw(has_next_cls, next_cls, {it, id},
                            out_tag, out_data) ? 1 : 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_math_range(
    int64_t start, int64_t end, int64_t step, int64_t line, int64_t col) {
  if (step == 0) {
    throw culebra::CulebraError("ValueError",
        "range() step must not be zero", line, col);
  }
  auto* current_cell = culebra_runtime_cell_new(TAG_LONG, start);
  auto* end_cell = culebra_runtime_cell_new(TAG_LONG, end);
  auto* step_cell = culebra_runtime_cell_new(TAG_LONG, step);
  return _iter_wrap_fast<&_math_range_fast_fn>(
      {current_cell, end_cell, step_cell});
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_iota(int64_t start,
                                                            int64_t end) {
  auto* r = culebra_runtime_array_new();
  for (int64_t i = start; i < end; i++) {
    culebra_runtime_array_push(r, /*tag Long*/ 2, i);
  }
  return r;
}

// --- Higher-order array helpers -------------------------------------------
//
// These invoke a user-supplied JIT-compiled closure per element. The call
// signatures below must match compile_function's emitted signature:
//
//   %Value fn(ptr __cls__, %Value this, %Value p1, ...)
//
// Closure arg ownership: each arg Value passed to the JIT'd function is
// consumed (released at function exit). So array elements must be retained
// before being handed to the closure.

// Type-check a higher-order callback argument. Returns the Closure or
// throws. Centralizes the identical preamble shared by all higher-order
// array runtime helpers.
inline JitClosure* _culebra_expect_callback(int8_t fn_tag, int64_t fn_data,
                                            size_t expected_arity,
                                            const char* method_name,
                                            int64_t line, int64_t col) {
  if (fn_tag != TAG_FUNC) {
    culebra::throw_type_error_at(line, col);
  }
  auto* fn = reinterpret_cast<JitClosure*>(fn_data);
  if (fn->arity != expected_arity) {
    throw culebra::CulebraError("TypeError", std::format(
        "type error: {} expects a {}-parameter function at {}:{}.",
        method_name, expected_arity, line, col), line, col);
  }
  return fn;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_map(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"map", line, col);
  auto* out = culebra_runtime_array_new();
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    culebra_runtime_array_push(out, r.tag, r.data);
  }
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_filter(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"filter", line, col);
  auto* out = culebra_runtime_array_new();
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) {
      culebra_runtime_value_retain(e.tag, e.data);
      culebra_runtime_array_push(out, e.tag, e.data);
    }
  }
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_for_each(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"for_each", line, col);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    _culebra_value_release_impl(r.tag, r.data);
  }
}

// find returns the first matching element (or nil) via out-params.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_find(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col,
    int8_t* out_tag, int64_t* out_data) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"find", line, col);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) {
      culebra_runtime_value_retain(e.tag, e.data);
      *out_tag = e.tag;
      *out_data = e.data;
      return;
    }
  }
  *out_tag = 0;
  *out_data = 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_array_any(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"any", line, col);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) return 1;
  }
  return 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_array_all(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"all", line, col);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (!keep) return 0;
  }
  return 1;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_flat_map(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"flat_map", line, col);
  auto* out = culebra_runtime_array_new();
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    if (r.tag != TAG_ARRAY) {
      _culebra_value_release_impl(r.tag, r.data);
      throw culebra::CulebraError("TypeError", std::format(
          "type error: flat_map callback must return an Array at {}:{}.",
          line, col), line, col);
    }
    auto* inner = reinterpret_cast<JitArray*>(r.data);
    for (size_t j = 0; j < inner->size; j++) {
      auto ie = inner->items[j];
      culebra_runtime_value_retain(ie.tag, ie.data);
      culebra_runtime_array_push(out, ie.tag, ie.data);
    }
    _culebra_value_release_impl(r.tag, r.data);
  }
  return out;
}

// sort_by: evaluates the key function on each element once, stable-sorts
// in place by those keys. Mutates. Keys are released on every exit path
// (including during a throw from the key comparison).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_sort_by(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"sort_by", line, col);
  std::vector<std::pair<JitValue, size_t>> keyed;
  keyed.reserve(arr->size);
  auto release_keys = [&] {
    for (auto& [k, _] : keyed) _culebra_value_release_impl(k.tag, k.data);
  };
  try {
    for (size_t i = 0; i < arr->size; i++) {
      auto e = arr->items[i];
      culebra_runtime_value_retain(e.tag, e.data);
      keyed.emplace_back(_culebra_invoke1(fn, e), i);
    }
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const auto& a, const auto& b) {
                       return _culebra_value_ord(
                           a.first.tag, a.first.data,
                           b.first.tag, b.first.data,
                           [](double x, double y) { return x < y; });
                     });
    std::vector<JitValue> sorted(arr->size);
    for (size_t i = 0; i < keyed.size(); i++) {
      sorted[i] = arr->items[keyed[i].second];
    }
    for (size_t i = 0; i < arr->size; i++) arr->items[i] = sorted[i];
  } catch (...) {
    release_keys();
    throw;
  }
  release_keys();
}

// reduce returns the final accumulator via out-params (avoids relying on
// cross-language struct-return ABI).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_reduce(
    JitArray* arr, int8_t init_tag, int64_t init_data, int8_t fn_tag,
    int64_t fn_data, int64_t line, int64_t col, int8_t* out_tag,
    int64_t* out_data) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 2, "reduce", line, col);
  JitValue acc = {init_tag, init_data};
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    acc = _culebra_invoke2(fn, acc, e);
  }
  *out_tag = acc.tag;
  *out_data = acc.data;
}

// Eager Array variants of sum/product/min/max. Same semantics as the
// iterator methods above; non-Long elements and empty min/max raise a
// type error at L:C.

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_array_sum(
    JitArray* arr, int64_t line, int64_t col) {
  int64_t acc = 0;
  for (size_t i = 0; i < arr->size; i++) {
    auto& e = arr->items[i];
    if (e.tag != TAG_LONG) {
      culebra::throw_type_error_at(line, col);
    }
    acc += e.data;
  }
  return acc;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_array_product(
    JitArray* arr, int64_t line, int64_t col) {
  int64_t acc = 1;
  for (size_t i = 0; i < arr->size; i++) {
    auto& e = arr->items[i];
    if (e.tag != TAG_LONG) {
      culebra::throw_type_error_at(line, col);
    }
    acc *= e.data;
  }
  return acc;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_array_min(
    JitArray* arr, int64_t line, int64_t col) {
  if (arr->size == 0) {
    throw culebra::CulebraError("ValueError", std::format(
        "min of empty Array at {}:{}.", line, col), line, col);
  }
  if (arr->items[0].tag != TAG_LONG) {
    culebra::throw_type_error_at(line, col);
  }
  int64_t best = arr->items[0].data;
  for (size_t i = 1; i < arr->size; i++) {
    auto& e = arr->items[i];
    if (e.tag != TAG_LONG) {
      culebra::throw_type_error_at(line, col);
    }
    if (e.data < best) best = e.data;
  }
  return best;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_array_max(
    JitArray* arr, int64_t line, int64_t col) {
  if (arr->size == 0) {
    throw culebra::CulebraError("ValueError", std::format(
        "max of empty Array at {}:{}.", line, col), line, col);
  }
  if (arr->items[0].tag != TAG_LONG) {
    culebra::throw_type_error_at(line, col);
  }
  int64_t best = arr->items[0].data;
  for (size_t i = 1; i < arr->size; i++) {
    auto& e = arr->items[i];
    if (e.tag != TAG_LONG) {
      culebra::throw_type_error_at(line, col);
    }
    if (e.data > best) best = e.data;
  }
  return best;
}

// Length in UTF-8 bytes of the next scalar at `offset`. Returns 0
// once `offset >= len`; on an invalid lead byte, returns 1 (emit the
// raw byte to avoid stalling the iterator). Mirrors the interpreter's
// String.iter semantics.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_utf8_scalar_len(
    const char* s, int64_t offset, int64_t len) {
  if (offset >= len) return 0;
  auto r = peg::codepoint_length(s + offset, len - offset);
  return r == 0 ? 1 : static_cast<int64_t>(r);
}

// Heap-copy `scalar_len` bytes from `s + offset` into a new String.
// Used by JIT for-in over String to yield one-scalar Strings.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_scalar_at(
    const char* s, int64_t offset, int64_t scalar_len) {
  return _culebra_heap_str(std::string_view(s + offset,
                                            static_cast<size_t>(scalar_len)));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_str_size(const char* s) {
  return static_cast<int64_t>(std::strlen(s));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_upper(
    const char* s) {
  return _culebra_heap_str(culebra::ascii_upper(std::string(s)));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_lower(
    const char* s) {
  return _culebra_heap_str(culebra::ascii_lower(std::string(s)));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_trim(
    const char* s) {
  auto trimmed = culebra::trim_ascii(std::string_view(s, std::strlen(s)));
  auto* buf = static_cast<char*>(std::malloc(trimmed.size() + 1));
  std::memcpy(buf, trimmed.data(), trimmed.size());
  buf[trimmed.size()] = '\0';
  return buf;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_str_split(
    const char* s, const char* sep) {
  auto* r = culebra_runtime_array_new();
  std::string_view sv(s);
  std::string_view sp(sep);
  auto push_view = [&](std::string_view piece) {
    auto* v = _culebra_heap_view(piece.data(), piece.size());
    culebra_runtime_array_push(r, TAG_STRINGVIEW,
                               reinterpret_cast<int64_t>(v));
  };
  if (sp.empty()) {
    push_view(sv);
    return r;
  }
  size_t pos = 0;
  while (true) {
    auto p = sv.find(sp, pos);
    if (p == std::string_view::npos) {
      push_view(sv.substr(pos));
      break;
    }
    push_view(sv.substr(pos, p - pos));
    pos = p + sp.size();
  }
  return r;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_str_contains(
    const char* s, const char* sub) {
  return std::strstr(s, sub) != nullptr;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_str_starts_with(
    const char* s, const char* prefix) {
  auto lp = std::strlen(prefix);
  return std::strncmp(s, prefix, lp) == 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_str_ends_with(
    const char* s, const char* suffix) {
  auto ls = std::strlen(s);
  auto lsuf = std::strlen(suffix);
  if (lsuf > ls) return false;
  return std::strncmp(s + (ls - lsuf), suffix, lsuf) == 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_slice(
    const char* s, int64_t start, int64_t end) {
  int64_t size = static_cast<int64_t>(std::strlen(s));
  if (start < 0) start += size;
  if (end < 0) end += size;
  if (start < 0) start = 0;
  if (start > size) start = size;
  if (end < start) end = start;
  if (end > size) end = size;
  auto len = static_cast<size_t>(end - start);
  auto* buf = static_cast<char*>(std::malloc(len + 1));
  std::memcpy(buf, s + start, len);
  buf[len] = '\0';
  return buf;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitStringView*
culebra_runtime_strlike_view(int8_t tag, int64_t data) {
  auto sv = _culebra_str_view(tag, data);
  return _culebra_heap_view(sv.data(), sv.size());
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitStringView*
culebra_runtime_strlike_slice_view(int8_t tag, int64_t data,
                                   int64_t start, int64_t end) {
  auto sv = _culebra_str_view(tag, data);
  int64_t size = static_cast<int64_t>(sv.size());
  if (start < 0) start += size;
  if (end < 0) end += size;
  if (start < 0) start = 0;
  if (start > size) start = size;
  if (end < start) end = start;
  if (end > size) end = size;
  return _culebra_heap_view(sv.data() + start,
                            static_cast<size_t>(end - start));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char*
culebra_runtime_strlike_to_cstr(int8_t tag, int64_t data) {
  if (tag == TAG_STRING) return reinterpret_cast<const char*>(data);
  auto* v = reinterpret_cast<const JitStringView*>(data);
  return _culebra_heap_str(std::string_view(v->ptr, v->len));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_pop(
    JitArray* arr, int8_t* out_tag, int64_t* out_data) {
  if (arr->size == 0) {
    *out_tag = 0;
    *out_data = 0;
    return;
  }
  auto& last = arr->items[arr->size - 1];
  *out_tag = last.tag;
  *out_data = last.data;
  arr->size--;
}

// Slice with negative indices and clamping. Returns a new array with retained
// elements (independent refcount).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_slice2(
    JitArray* src, int64_t start, int64_t end) {
  int64_t size = static_cast<int64_t>(src->size);
  if (start < 0) start += size;
  if (end < 0) end += size;
  if (start < 0) start = 0;
  if (start > size) start = size;
  if (end < start) end = start;
  if (end > size) end = size;
  auto* r = culebra_runtime_array_new();
  for (int64_t i = start; i < end; i++) {
    auto& e = src->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    culebra_runtime_array_push(r, e.tag, e.data);
  }
  return r;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_array_join(
    JitArray* arr, const char* sep) {
  std::string out;
  for (size_t i = 0; i < arr->size; i++) {
    if (i > 0) out += sep;
    auto tag = arr->items[i].tag;
    auto data = arr->items[i].data;
    if (tag == TAG_STRING || tag == TAG_STRINGVIEW) {
      out += _culebra_str_view(tag, data);
    } else {
      out += _culebra_value_to_str_impl(tag, data);
    }
  }
  return _culebra_heap_str(out);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_array_index_of(
    JitArray* arr, int8_t tag, int64_t data) {
  for (size_t i = 0; i < arr->size; i++) {
    if (_culebra_value_equal(arr->items[i].tag, arr->items[i].data, tag, data))
      return static_cast<int64_t>(i);
  }
  return -1;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_array_contains(
    JitArray* arr, int8_t tag, int64_t data) {
  return culebra_runtime_array_index_of(arr, tag, data) >= 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_reverse(JitArray* arr) {
  if (arr->size < 2) return;
  for (size_t i = 0, j = arr->size - 1; i < j; i++, j--) {
    auto tmp = arr->items[i];
    arr->items[i] = arr->items[j];
    arr->items[j] = tmp;
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_object_keys(
    JitObject* obj) {
  auto* r = culebra_runtime_array_new();
  // Walk key_order when populated so String and non-String keys come
  // out interleaved in insertion order. Fall back to the shape walk
  // for objects built via direct slot append (class instances).
  if (obj->key_order && !obj->key_order->empty()) {
    for (auto& k : *obj->key_order) {
      culebra_runtime_value_retain(k.tag, k.data);
      culebra_runtime_array_push(r, k.tag, k.data);
    }
    return r;
  }
  for (size_t i = 0; obj->shape && i < obj->shape->names.size(); i++) {
    auto& k = obj->shape->names[i];
    auto* buf = _culebra_heap_str(k);
    culebra_runtime_array_push(r, TAG_STRING, reinterpret_cast<int64_t>(buf));
  }
  return r;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_remove(
    JitObject* obj, const char* key) {
  auto idx = obj->find_slot(key);
  if (idx == static_cast<size_t>(-1)) return;
  _culebra_value_release_impl(obj->slots[idx].value.tag,
                              obj->slots[idx].value.data);
  obj->erase(key);
  if (std::string_view(key) == "drop") obj->has_drop = false;
}

// Drop a key from `key_order` if present. Matches by JitValueEq so
// numerically-equivalent keys (e.g. `1`/`1.0`/`true`) all resolve to
// the same slot — same rule the AnyKeyMap uses on insert.
inline void _key_order_erase(JitObject* obj, const JitValue& key) {
  if (!obj->key_order) return;
  JitValueEq eq;
  auto& ko = *obj->key_order;
  for (auto it = ko.begin(); it != ko.end(); ++it) {
    if (eq(*it, key)) {
      // Refcounted (Tuple) keys hold a +1 here — release before erasing.
      if (_GcTracker::is_refcounted_value_tag(it->tag)) {
        _culebra_value_release_impl(it->tag, it->data);
      }
      ko.erase(it);
      return;
    }
  }
}

// Generic remove. String keys go through the shape (TAG_STRING data
// is a borrowed cstring, no refcount to manage); non-String keys go
// through the sidecar where the caller's +1 to the lookup key is
// consumed, and the stored map-entry key's +1 (transferred from the
// original `object_set_any` insert) is also released before erase.
// `_key_order_erase` drops the key_order entry's +1 separately.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_remove_any(
    JitObject* obj, int8_t tag, int64_t data) {
  if (tag == TAG_STRING) {
    auto* cstr = reinterpret_cast<const char*>(data);
    if (obj->find_slot(cstr) != static_cast<size_t>(-1)) {
      culebra_runtime_object_remove(obj, cstr);
      _key_order_erase(obj, {TAG_STRING, data});
    }
    return;
  }
  if (!obj->non_string_props) {
    _culebra_value_release_impl(tag, data);
    return;
  }
  JitValue key{tag, data};
  auto it = obj->non_string_props->find(key);
  if (it == obj->non_string_props->end()) {
    _culebra_value_release_impl(tag, data);
    return;
  }
  _culebra_value_release_impl(it->second.value.tag, it->second.value.data);
  _culebra_value_release_impl(it->first.tag, it->first.data);
  obj->non_string_props->erase(it);
  _key_order_erase(obj, key);
  _culebra_value_release_impl(tag, data);
}

}  // extern "C" (close briefly for C++ impl helpers)

// --- Reference counting (C++ impl helpers, not extern "C") ---
//
// Only heap-allocated refcounted types have refcount headers:
//   TAG_FUNC (closure), TAG_ARRAY, TAG_OBJECT, and JitCell (internal).
// Strings (TAG_STRING) are NOT refcounted in this implementation and leak.
// Nil/Bool/Long are value types and have no refcount.

inline void _culebra_cell_release(JitCell* c);

// RAII drop: if `o` has a 0-arg `drop` Function property, invoke it
// with `this` bound to `o` before any child values are released.
// Called from the normal refcount-0 path and from the cycle collector.
// The contract (drop must be a 0-arg Function) is validated at
// assignment time, so a mis-shaped drop is silently skipped here as a
// belt-and-braces check.
//
// Refcount trick (matches interpreter's shared_ptr + no-op deleter):
// bump high enough that the drop body's function-frame release of its
// owned `this` slot — plus any retain/release pairs in the body —
// can't drive refcount back to 0 and re-enter destruction. The frame
// release subtracts 1, so 2 is the minimum safe baseline. After drop
// returns we slam the count to 0; if the body resurrected `o` by
// storing it somewhere, the caller owns the resulting dangling
// reference (matches interp's documented warning).
inline void _culebra_call_drop_if_present(JitObject* o) {
  if (!o || !o->has_drop) return;
  // Walks proto so class-sugar instances find their inherited `drop`.
  auto* entry = _find_property(o, "drop");
  if (!entry) return;
  const auto& v = entry->value;
  if (v.tag != GC_TAG_FUNC) return;
  auto* cls = reinterpret_cast<JitClosure*>(v.data);
  if (!cls || cls->arity != 0) return;

  o->refcount = 2;
  JitValue this_val{GC_TAG_OBJECT, reinterpret_cast<int64_t>(o)};
  try {
    auto r = reinterpret_cast<JitFn>(cls->fn_ptr)(cls, this_val, 0,
                                                  nullptr);
    _culebra_value_release_impl(r.tag, r.data);
  } catch (const std::exception& e) {
    std::cerr << "drop: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "drop: unknown error" << std::endl;
  }
  o->refcount = 0;
}

inline void _culebra_value_release_impl(int8_t tag, int64_t data) {
  if (data == 0) return;
  switch (tag) {
    case GC_TAG_FUNC: {
      auto* c = reinterpret_cast<JitClosure*>(data);
      if (--c->refcount == 0) {
        for (size_t i = 0; i < c->n_captures; i++) {
          _culebra_cell_release(c->captures[i]);
        }
        std::free(c->captures);
        _gc().remove(c, GC_TAG_FUNC);
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
        _gc().remove(a, tag);
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
            if (_GcTracker::is_refcounted_value_tag(k.tag)) {
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
        _gc().remove(o, GC_TAG_OBJECT);
        delete o;
      }
      break;
    }
#ifndef CULEBRA_RT_NO_TENSOR
    case GC_TAG_TENSOR: {
      auto* t = reinterpret_cast<JitTensor*>(data);
      if (--t->refcount == 0) {
        _gc().remove(t, GC_TAG_TENSOR);
        delete t;  // ~JitTensor releases the shared_ptr<TensorImpl>
      }
      break;
    }
#endif
    case GC_TAG_SET: {
      auto* s = reinterpret_cast<JitSet*>(data);
      if (--s->refcount == 0) {
        for (auto& m : s->members) {
          _culebra_value_release_impl(m.tag, m.data);
        }
        delete s->index;
        _gc().remove(s, GC_TAG_SET);
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
    _gc().remove(c, GC_TAG_CELL);
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
inline constexpr auto array_size          = "culebra_runtime_array_size";
inline constexpr auto tensor_zeros        = "culebra_runtime_tensor_zeros";
inline constexpr auto tensor_ones         = "culebra_runtime_tensor_ones";
inline constexpr auto tensor_randn        = "culebra_runtime_tensor_randn";
inline constexpr auto tensor_from         = "culebra_runtime_tensor_from";
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
inline constexpr auto tensor_dot          = "culebra_runtime_tensor_dot";
inline constexpr auto tensor_from_csv     = "culebra_runtime_tensor_from_csv";
inline constexpr auto tensor_unary        = "culebra_runtime_tensor_unary";
inline constexpr auto tensor_linear_sigmoid = "culebra_runtime_tensor_linear_sigmoid";
inline constexpr auto array_slice         = "culebra_runtime_array_slice";
inline constexpr auto array_slice2        = "culebra_runtime_array_slice2";
inline constexpr auto array_sort_by       = "culebra_runtime_array_sort_by";
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
inline constexpr auto json_stringify_kw   = "culebra_runtime_json_stringify_kw";
inline constexpr auto json_parse          = "culebra_runtime_json_parse";
inline constexpr auto json_parse_kw       = "culebra_runtime_json_parse_kw";
// Trailing underscore on `throw_` dodges C++ keyword collision.
inline constexpr auto cell_new            = "culebra_runtime_cell_new";
inline constexpr auto cell_release        = "culebra_runtime_cell_release";
inline constexpr auto cell_retain         = "culebra_runtime_cell_retain";
inline constexpr auto closure_new         = "culebra_runtime_closure_new";
inline constexpr auto register_param_meta = "culebra_runtime_register_param_meta";
inline constexpr auto fn_introspect_get    = "culebra_runtime_fn_introspect_get";
inline constexpr auto call_with_kwargs    = "culebra_runtime_call_with_kwargs";
inline constexpr auto debugger_break      = "culebra_runtime_debugger_break";
inline constexpr auto defer_mark          = "culebra_runtime_defer_mark";
inline constexpr auto defer_push          = "culebra_runtime_defer_push";
inline constexpr auto defer_run_to        = "culebra_runtime_defer_run_to";
inline constexpr auto div_zero            = "culebra_runtime_div_zero";
inline constexpr auto input               = "culebra_runtime_input";
inline constexpr auto math_pow            = "culebra_runtime_math_pow";
inline constexpr auto math_log            = "culebra_runtime_math_log";
inline constexpr auto math_exp            = "culebra_runtime_math_exp";
inline constexpr auto math_sqrt           = "culebra_runtime_math_sqrt";
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
inline constexpr auto object_get          = "culebra_runtime_object_get";
inline constexpr auto object_get_ic       = "culebra_runtime_object_get_ic";
inline constexpr auto object_has          = "culebra_runtime_object_has";
inline constexpr auto object_class_matches
    = "culebra_runtime_object_class_matches";
inline constexpr auto object_has_value    = "culebra_runtime_object_has_value";
inline constexpr auto object_keys         = "culebra_runtime_object_keys";
inline constexpr auto object_new          = "culebra_runtime_object_new";
inline constexpr auto object_remove       = "culebra_runtime_object_remove";
inline constexpr auto object_remove_any   = "culebra_runtime_object_remove_any";
inline constexpr auto build_class_instance
    = "culebra_runtime_build_class_instance";
inline constexpr auto build_class_meta
    = "culebra_runtime_build_class_meta";
inline constexpr auto object_set          = "culebra_runtime_object_set";
inline constexpr auto object_set_fast     = "culebra_runtime_object_set_fast";
inline constexpr auto object_set_ic       = "culebra_runtime_object_set_ic";
inline constexpr auto object_set_any      = "culebra_runtime_object_set_any";
inline constexpr auto object_get_any      = "culebra_runtime_object_get_any";
inline constexpr auto object_has_any      = "culebra_runtime_object_has_any";
inline constexpr auto check_well_known_prop =
    "culebra_runtime_check_well_known_prop";
inline constexpr auto object_size         = "culebra_runtime_object_size";
inline constexpr auto print               = "culebra_runtime_print";
inline constexpr auto puts                = "culebra_runtime_puts";
inline constexpr auto iota                = "culebra_runtime_iota";
inline constexpr auto math_range           = "culebra_runtime_math_range";
inline constexpr auto check_pos_count      = "culebra_runtime_check_pos_count";
inline constexpr auto repl_get             = "culebra_runtime_repl_get";
inline constexpr auto repl_set             = "culebra_runtime_repl_set";
inline constexpr auto iter_collect         = "culebra_runtime_iter_collect";
inline constexpr auto iter_count           = "culebra_runtime_iter_count";
inline constexpr auto iter_for_each        = "culebra_runtime_iter_for_each";
inline constexpr auto iter_reduce          = "culebra_runtime_iter_reduce";
inline constexpr auto iter_find            = "culebra_runtime_iter_find";
inline constexpr auto iter_any             = "culebra_runtime_iter_any";
inline constexpr auto iter_all             = "culebra_runtime_iter_all";
inline constexpr auto iter_max             = "culebra_runtime_iter_max";
inline constexpr auto iter_min             = "culebra_runtime_iter_min";
inline constexpr auto iter_product         = "culebra_runtime_iter_product";
inline constexpr auto iter_sum             = "culebra_runtime_iter_sum";
inline constexpr auto iter_map             = "culebra_runtime_iter_map";
inline constexpr auto iter_filter          = "culebra_runtime_iter_filter";
inline constexpr auto iter_take            = "culebra_runtime_iter_take";
inline constexpr auto iter_skip            = "culebra_runtime_iter_skip";
inline constexpr auto iter_take_while      = "culebra_runtime_iter_take_while";
inline constexpr auto iter_chain           = "culebra_runtime_iter_chain";
inline constexpr auto iter_zip             = "culebra_runtime_iter_zip";
inline constexpr auto iter_enumerate       = "culebra_runtime_iter_enumerate";
inline constexpr auto iter_flat_map        = "culebra_runtime_iter_flat_map";
inline constexpr auto iter_advance         = "culebra_runtime_iter_advance";
inline constexpr auto str_code_points      = "culebra_runtime_str_code_points";
inline constexpr auto str_graphemes        = "culebra_runtime_str_graphemes";
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
inline constexpr auto str_upper           = "culebra_runtime_str_upper";
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
inline constexpr auto write_file          = "culebra_runtime_write_file";
}  // namespace rt

// --- JIT compiler implementation ---

struct JIT {
  // Per-variable slot in a scope: either a stack-allocated value or a cell
  // (heap-allocated box for closure capture).
  struct VarSlot {
    enum Kind { Stack, Cell };
    Kind kind;
    llvm::AllocaInst* alloca;  // Stack: holds Value. Cell: holds Cell pointer.
    // True when the slot owns the +1 ref it holds — its value will be
    // released on scope exit. False for borrowed views: capture cells
    // (the closure object owns the cell) and any other slot whose ref
    // count is managed elsewhere. Required to keep callers honest;
    // every binding has a clear ownership story.
    bool owned;
    // True if declared via `let mut`. Reassigning a non-mut binding
    // raises ImmutableError, matching the interp; forward-ref
    // pre-allocated cells default to false and get updated when the
    // eventual `let mut` lands.
    bool mut = false;
  };

  // Analysis result for a function (including top-level __culebra_main).
  struct FuncInfo {
    std::vector<std::string> free_vars;   // captured from outer
    // Parallel to free_vars: mut flag of the outer slot at the moment the
    // closure was instantiated. Populated by emit_closure_build, consumed
    // by the inner function's free-var binding so ImmutableError fires on
    // writes to captured non-mut bindings.
    std::vector<bool> free_var_mut;
    std::set<std::string> captured_locals;  // my locals captured by nested
    // EH/defer emission flags (populated by scan_eh_defer):
    bool has_eh = false;        // contains TRY or any scope with defers
    bool has_fn_defer = false;  // contains DEFER directly in fn body
    // True if the function body references the auto-bound `__ARGS__`
    // identifier (overflow args Array). When false, the prologue skips
    // building the Array entirely — saves a heap allocation per call
    // for the common no-varargs case.
    bool uses_args = false;
  };

  // LEXICAL_SCOPE nodes that contain a DEFER within their own scope level
  // (not crossing nested LEXICAL_SCOPE / FUNCTION / DEFER). Scopes not in
  // this set skip emitting scope.mark / scope.cleanup landingpad.
  std::unordered_set<const peg::Ast*> scope_has_defer_;

  // Variable scoping: `slots` for lookup, `order` for LIFO release.
  // `std::map` iterators are stable under try_emplace, so `order` can
  // hold them directly — no second string copy and no per-release
  // lookup.
  struct Scope {
    using Slots = std::map<std::string, VarSlot>;
    Slots slots;
    std::vector<Slots::iterator> order;
    // `let f = fn (...) {...}` records the FUNCTION AST here so a
    // later `f(x, y: 2)` can resolve kwargs against `f`'s parameter
    // list at compile time. Only same-scope direct bindings populate
    // this map; captured / reassigned closures do not.
    std::map<std::string, const peg::Ast*> fn_asts;
  };

  // RAII snapshot of compiler per-function state. Entering a nested
  // LLVM function body (compile_function / compile_defer) constructs
  // one; the dtor (or an explicit `restore()` before the caller's
  // closure-build) restores the outer context. Ctor resets all fields
  // to safe defaults — outer's lpad, fn-mark, scopes, etc. are
  // intentionally not inherited since they live in a different LLVM
  // function. `restore()` is idempotent.
  struct CompilerStateSaver {
    JIT* jit;
    llvm::BasicBlock* insert_block;
    std::vector<Scope> scopes;
    const FuncInfo* info;
    llvm::Value* closure_arg;
    std::string_view return_type;
    llvm::BasicBlock* lpad;
    llvm::Value* fn_defer_mark;
    bool is_repl_top_level;

    explicit CompilerStateSaver(JIT& j)
        : jit(&j),
          insert_block(j.builder_.GetInsertBlock()),
          scopes(std::exchange(j.scopes_, {})),
          info(std::exchange(j.current_info_, nullptr)),
          closure_arg(std::exchange(j.current_closure_arg_, nullptr)),
          return_type(std::exchange(j.current_return_type_, {})),
          lpad(std::exchange(j.current_lpad_, nullptr)),
          fn_defer_mark(std::exchange(j.current_fn_defer_mark_, nullptr)),
          is_repl_top_level(
              std::exchange(j.is_repl_top_level_, false)) {}

    void restore() {
      if (!jit) return;
      // Caller is expected to pop every scope it pushed; anything left
      // behind signals a push/pop imbalance that would silently leak.
      assert(jit->scopes_.empty() && "unbalanced push_scope/pop_scope");
      jit->current_fn_defer_mark_ = fn_defer_mark;
      jit->current_lpad_ = lpad;
      jit->current_return_type_ = return_type;
      jit->current_closure_arg_ = closure_arg;
      jit->current_info_ = info;
      jit->scopes_ = std::move(scopes);
      jit->builder_.SetInsertPoint(insert_block);
      jit->is_repl_top_level_ = is_repl_top_level;
      jit = nullptr;
    }

    ~CompilerStateSaver() { restore(); }

    CompilerStateSaver(const CompilerStateSaver&) = delete;
    CompilerStateSaver& operator=(const CompilerStateSaver&) = delete;
    CompilerStateSaver(CompilerStateSaver&&) = delete;
    CompilerStateSaver& operator=(CompilerStateSaver&&) = delete;
  };

  // When inside a `try { ... }` region, points at the landingpad BB
  // that catches `CulebraException` for that region. Any call emitted
  // while this is non-null is emitted as `invoke` with this as the
  // unwind destination, so a user `throw` propagates back to the
  // nearest enclosing `try`. Nested try blocks save/restore.
  llvm::BasicBlock* current_lpad_ = nullptr;

  // Targets for the innermost enclosing loop. `break` jumps to the
  // break target (after the loop); `continue` jumps to the continue
  // target (loop-header / increment). Loops push a frame on entry and
  // pop on exit; nested loops stack correctly.
  struct LoopBlocks {
    llvm::BasicBlock* continue_target;
    llvm::BasicBlock* break_target;
  };
  std::vector<LoopBlocks> loop_stack_;

  // Unified call-site emitter: `invoke` if inside a try, else `call`.
  // All JIT-generated call sites (runtime functions, user functions,
  // closures) go through this so user `throw` propagates uniformly.
  llvm::CallBase* emit_call(llvm::FunctionCallee callee,
                            llvm::ArrayRef<llvm::Value*> args,
                            const llvm::Twine& name = "") {
    if (current_lpad_) {
      auto fn = builder_.GetInsertBlock()->getParent();
      auto contBB = llvm::BasicBlock::Create(ctx_, "call.cont", fn);
      auto inv = builder_.CreateInvoke(callee, contBB, current_lpad_, args,
                                       name);
      builder_.SetInsertPoint(contBB);
      return inv;
    }
    return builder_.CreateCall(callee, args, name);
  }

  // Indirect-call overload (function pointer + explicit FunctionType).
  llvm::CallBase* emit_call(llvm::FunctionType* fty, llvm::Value* fnPtr,
                            llvm::ArrayRef<llvm::Value*> args,
                            const llvm::Twine& name = "") {
    if (current_lpad_) {
      auto fn = builder_.GetInsertBlock()->getParent();
      auto contBB = llvm::BasicBlock::Create(ctx_, "call.cont", fn);
      auto inv = builder_.CreateInvoke(fty, fnPtr, contBB, current_lpad_, args,
                                       name);
      builder_.SetInsertPoint(contBB);
      return inv;
    }
    return builder_.CreateCall(fty, fnPtr, args, name);
  }

  // Re-raise the currently-handled exception (inside a catch-all
  // cleanup landingpad). If `outerLpad` is non-null, invoke
  // `__cxa_rethrow` with it as the unwind target so the exception
  // reaches that outer lpad within the same LLVM function; otherwise
  // a plain call lets the exception propagate out to the caller.
  void emit_rethrow(llvm::BasicBlock* outerLpad) {
    auto rethrowFn = module_->getOrInsertFunction(
        "__cxa_rethrow", builder_.getVoidTy());
    auto fn = builder_.GetInsertBlock()->getParent();
    if (outerLpad) {
      auto deadBB = llvm::BasicBlock::Create(ctx_, "rethrow.dead", fn);
      builder_.CreateInvoke(rethrowFn, deadBB, outerLpad, {});
      builder_.SetInsertPoint(deadBB);
    } else {
      builder_.CreateCall(rethrowFn, {});
    }
    builder_.CreateUnreachable();
  }

  // Build a catch-all cleanup landingpad at the current insertion
  // point: catches any in-flight exception, runs the scope's defers
  // back to `mark`, and re-raises so the exception keeps propagating
  // (to `outerLpad` if given, otherwise out of the current function).
  void emit_cleanup_landingpad(llvm::BasicBlock* cleanupBB,
                               llvm::Value* mark,
                               llvm::BasicBlock* outerLpad,
                               const char* excName) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    builder_.SetInsertPoint(cleanupBB);
    auto lpadTy = llvm::StructType::get(ptrTy, builder_.getInt32Ty());
    auto lpad = builder_.CreateLandingPad(lpadTy, 1, excName);
    lpad->addClause(llvm::ConstantPointerNull::get(ptrTy));  // catch-all
    auto excPtr = builder_.CreateExtractValue(lpad, {0});
    builder_.CreateCall(
        module_->getOrInsertFunction("__cxa_begin_catch", ptrTy, ptrTy),
        {excPtr});
    builder_.CreateCall(
        module_->getFunction(rt::defer_run_to), {mark});
    emit_rethrow(outerLpad);
  }

  // Itanium C++ personality. Same attribute used by -fexceptions output.
  llvm::Constant* get_personality_fn() {
    auto fty = llvm::FunctionType::get(builder_.getInt32Ty(), {}, true);
    auto callee = module_->getOrInsertFunction("__gxx_personality_v0", fty);
    return llvm::cast<llvm::Constant>(callee.getCallee());
  }

  // Process-wide LLVM target init. Concurrent callers race on the
  // built-in target registry, so guard with std::call_once. Each
  // JIT::run() goes through this before touching ORC.
  static void ensure_native_target_init() {
    static std::once_flag flag;
    std::call_once(flag, []() {
      llvm::InitializeNativeTarget();
      llvm::InitializeNativeTargetAsmPrinter();
    });
  }

  // Cross-compile target init. With CULEBRA_LLVM_ALL_TARGETS the driver
  // links every LLVM backend (~31 MB) and registers them all so that
  // `culebra build --target=<triple>` accepts any triple. Without the
  // option the driver only links Native + WebAssembly (default), so
  // only those two are registered here. Lazy + idempotent; only the
  // AOT path pays this cost.
  static void ensure_all_targets_init() {
    static std::once_flag flag;
    std::call_once(flag, []() {
#ifdef CULEBRA_LLVM_ALL_TARGETS
      llvm::InitializeAllTargetInfos();
      llvm::InitializeAllTargets();
      llvm::InitializeAllTargetMCs();
      llvm::InitializeAllAsmPrinters();
      llvm::InitializeAllAsmParsers();
#else
      llvm::InitializeNativeTarget();
      llvm::InitializeNativeTargetAsmPrinter();
      llvm::InitializeNativeTargetAsmParser();
      LLVMInitializeWebAssemblyTargetInfo();
      LLVMInitializeWebAssemblyTarget();
      LLVMInitializeWebAssemblyTargetMC();
      LLVMInitializeWebAssemblyAsmPrinter();
      LLVMInitializeWebAssemblyAsmParser();
#endif
    });
  }

  // True if the AST contains any `*` keyword-only separator. Used to
  // elide the dynamic-callee kw-only runtime guard when no function
  // in the program declares kw-only params (the common case).
  static inline bool scan_for_kw_only_marker(const peg::Ast& node) {
    if (culebra::is_kw_only_sep(node)) return true;
    for (auto& c : node.nodes) {
      if (scan_for_kw_only_marker(*c)) return true;
    }
    return false;
  }

  // Build a fresh, ready-to-use LLJIT instance with the host's process
  // symbols available. Shared between one-shot `exec` (script mode)
  // and the persistent REPL JIT.
  static std::unique_ptr<llvm::orc::LLJIT> create_jit_instance() {
    using namespace llvm;
    auto jit = cantFail(orc::LLJITBuilder().create());
    auto& jd = jit->getMainJITDylib();
    auto gen = cantFail(
        orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix()));
    jd.addGenerator(std::move(gen));
    return jit;
  }

  static inline void run(const std::shared_ptr<peg::Ast>& ast,
                         bool emit_llvm = false, bool debug = false,
                         int opt_level = 2) {
    using namespace llvm;

    ensure_native_target_init();

    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>("culebra", *ctx);
    // Apply the host's data layout so struct field offsets, alignments
    // and pointer sizes match the C++ runtime we're linking against.
    // Without this, a {i8, i64} Value is laid out with i64 at offset 1
    // (no padding), diverging from C++'s 16-byte layout the runtime
    // uses for JitValue — any alloca-backed arg-slab would break.
    {
      llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
      std::string err;
      auto* target = llvm::TargetRegistry::lookupTarget(triple, err);
      if (target) {
        std::unique_ptr<llvm::TargetMachine> tm(
            target->createTargetMachine(triple, "", "", {}, {}));
        if (tm) {
          mod->setDataLayout(tm->createDataLayout());
          mod->setTargetTriple(triple);
        }
      }
    }
    IRBuilder<> builder(*ctx);

    JIT jit(ctx.get(), mod.get(), builder);
    jit.debug_enabled_ = debug;
    jit.declare_runtime_functions();

    // Pre-pass: free variable analysis for the whole program
    jit.main_info_ = jit.analyze_program(*ast);
    jit.current_info_ = &jit.main_info_;
    jit.any_kw_only_in_program_ = scan_for_kw_only_marker(*ast);

    // Create __culebra_main: void ()
    auto mainFnType = FunctionType::get(builder.getVoidTy(), {}, false);
    auto mainFn = Function::Create(mainFnType, GlobalValue::ExternalLinkage,
                                   "__culebra_main", mod.get());
    if (jit.main_info_.has_eh) {
      mainFn->setPersonalityFn(jit.get_personality_fn());
    }

    auto entryBB = BasicBlock::Create(*ctx, "entry", mainFn);
    builder.SetInsertPoint(entryBB);

    jit.push_scope();

    // Top-level defers run on normal completion; on an uncaught throw
    // they are skipped (wrap in a `{}` or try/catch for throw-path
    // cleanup).
    llvm::Value* mainMark = nullptr;
    if (jit.main_info_.has_fn_defer) {
      mainMark = builder.CreateCall(
          mod->getFunction(rt::defer_mark), {}, "main.mark");
      jit.current_fn_defer_mark_ = mainMark;
    }

    jit.pre_allocate_forward_refs(*ast);
    jit.compile(*ast);

    if (!builder.GetInsertBlock()->getTerminator()) {
      if (mainMark) {
        builder.CreateCall(
            mod->getFunction(rt::defer_run_to), {mainMark});
      }
      jit.release_all_scopes_for_exit();
      builder.CreateRetVoid();
    }

    jit.current_fn_defer_mark_ = nullptr;
    jit.pop_scope();
    jit.current_info_ = nullptr;

    verifyFunction(*mainFn);

    if (opt_level > 0) {
      optimize_module(*mod, opt_level);
    }

    if (emit_llvm) {
      mod->print(outs(), nullptr);
    } else {
      exec(std::move(ctx), std::move(mod));
    }
  }

  // Multi-module variant. `modules` comes from ModuleLoader in
  // topological order (deps first, entry last). Dependencies compile
  // into a private scope inside __culebra_main, build their export
  // Object, and hand it to `culebra_runtime_module_register`. The
  // entry module compiles last and shares the caller-facing scope.
  // IMPORT_STMT sites in any module fetch from the module table via
  // `culebra_runtime_module_get`.
  static inline void run_modules(
      const std::vector<LoadedModule>& orig_modules,
      bool emit_llvm = false, bool debug = false, int opt_level = 2) {
    using namespace llvm;
    if (orig_modules.empty()) return;

    // Prepend built-in trait preamble (mirrors interp's
    // interpret_modules). Registers Stringer / Eq / Comparable before
    // any user code runs; default-method closures compile into the
    // same JIT module so dispatch works from the first call.
    std::vector<LoadedModule> modules;
    modules.reserve(orig_modules.size() + 1);
    if (auto pre_ast = culebra::parse_builtin_traits_preamble()) {
      LoadedModule preamble;
      preamble.abs_path = "<builtin>";
      preamble.source = std::make_shared<std::string>(
          std::string(culebra::builtin_traits_preamble()));
      preamble.ast = pre_ast;
      modules.push_back(std::move(preamble));
    }
    for (const auto& m : orig_modules) modules.push_back(m);

    ensure_native_target_init();
    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>("culebra", *ctx);
    {
      llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
      std::string err;
      auto* target = llvm::TargetRegistry::lookupTarget(triple, err);
      if (target) {
        std::unique_ptr<llvm::TargetMachine> tm(
            target->createTargetMachine(triple, "", "", {}, {}));
        if (tm) {
          mod->setDataLayout(tm->createDataLayout());
          mod->setTargetTriple(triple);
        }
      }
    }
    IRBuilder<> builder(*ctx);

    JIT jit(ctx.get(), mod.get(), builder);
    jit.debug_enabled_ = debug;
    jit.declare_runtime_functions();

    // Per-module FuncInfo. Pre-compute before any IR emission so we
    // know the aggregate EH / fn-defer flags for the main fn header.
    std::vector<FuncInfo> infos;
    infos.reserve(modules.size());
    bool any_eh = false;
    bool any_kw_only = false;
    for (const auto& m : modules) {
      infos.push_back(jit.analyze_program(*m.ast));
      any_eh = any_eh || infos.back().has_eh;
      any_kw_only = any_kw_only || scan_for_kw_only_marker(*m.ast);
    }
    jit.any_kw_only_in_program_ = any_kw_only;

    auto mainFnType = FunctionType::get(builder.getVoidTy(), {}, false);
    auto mainFn = Function::Create(mainFnType, GlobalValue::ExternalLinkage,
                                    "__culebra_main", mod.get());
    if (any_eh) mainFn->setPersonalityFn(jit.get_personality_fn());

    auto entryBB = BasicBlock::Create(*ctx, "entry", mainFn);
    builder.SetInsertPoint(entryBB);

    // Dependencies. Each module compiles inside a fresh scope so its
    // top-level bindings don't leak into the entry; the export Object
    // is registered in the module table before the scope closes.
    for (size_t i = 0; i + 1 < modules.size(); ++i) {
      const auto& m = modules[i];
      jit.current_module_path_ = m.abs_path;
      jit.main_info_ = std::move(infos[i]);
      jit.current_info_ = &jit.main_info_;
      jit.push_scope();
      // Per-dep defer mark so top-level defers in the dep fire at
      // dep-end, matching interp's interpret_modules.
      llvm::Value* depMark = nullptr;
      if (jit.main_info_.has_fn_defer) {
        depMark = builder.CreateCall(
            mod->getFunction(rt::defer_mark), {}, "dep.mark");
        jit.current_fn_defer_mark_ = depMark;
      }
      jit.pre_allocate_forward_refs(*m.ast);
      jit.compile(*m.ast);
      jit.emit_build_and_register_export(*m.ast, m.abs_path.string());
      if (depMark) {
        builder.CreateCall(
            mod->getFunction(rt::defer_run_to), {depMark});
      }
      jit.current_fn_defer_mark_ = nullptr;
      jit.pop_scope();
      jit.current_info_ = nullptr;
    }

    // Entry module — shares the rest of __culebra_main (defer mark,
    // top-level scope cleanup, ret void).
    const auto& entry = modules.back();
    jit.current_module_path_ = entry.abs_path;
    jit.main_info_ = std::move(infos.back());
    jit.current_info_ = &jit.main_info_;
    jit.push_scope();
    llvm::Value* mainMark = nullptr;
    if (jit.main_info_.has_fn_defer) {
      mainMark = builder.CreateCall(
          mod->getFunction(rt::defer_mark), {}, "main.mark");
      jit.current_fn_defer_mark_ = mainMark;
    }
    jit.pre_allocate_forward_refs(*entry.ast);
    jit.compile(*entry.ast);
    if (!builder.GetInsertBlock()->getTerminator()) {
      if (mainMark) {
        builder.CreateCall(
            mod->getFunction(rt::defer_run_to), {mainMark});
      }
      jit.release_all_scopes_for_exit();
      builder.CreateRetVoid();
    }
    jit.current_fn_defer_mark_ = nullptr;
    jit.pop_scope();
    jit.current_info_ = nullptr;

    verifyFunction(*mainFn);
    if (opt_level > 0) optimize_module(*mod, opt_level);
    if (emit_llvm) {
      mod->print(outs(), nullptr);
    } else {
      exec(std::move(ctx), std::move(mod));
    }
  }

  // AOT codegen path. Mirrors `run()` up through `compile(ast)` but
  // emits an object file via TargetMachine instead of handing the
  // module to ORC. Also adds a `int main(int, char**)` IR function
  // that calls `culebra_aot_bootstrap` (defined in libculebra_rt.a)
  // with the program's `__culebra_main` as the user-main pointer.
  // Returns 0 on success, non-zero on emission failure.
  static inline int build_object(const std::shared_ptr<peg::Ast>& ast,
                                 const std::string& out_path,
                                 int opt_level = 2,
                                 bool emit_llvm = false,
                                 const std::string& target_triple = "") {
    using namespace llvm;

    if (target_triple.empty()) {
      ensure_native_target_init();
    } else {
      ensure_all_targets_init();
    }

    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>("culebra", *ctx);

    llvm::Triple triple(target_triple.empty()
                            ? llvm::sys::getDefaultTargetTriple()
                            : target_triple);
    std::string err;
    auto* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) {
      std::fprintf(stderr,
                   "culebra build: target lookup failed for '%s': %s\n",
                   triple.str().c_str(), err.c_str());
      return 1;
    }
    std::unique_ptr<llvm::TargetMachine> tm(
        target->createTargetMachine(triple, "", "", {}, {}));
    if (!tm) {
      std::fprintf(stderr,
                   "culebra build: target machine creation failed\n");
      return 1;
    }
    mod->setDataLayout(tm->createDataLayout());
    mod->setTargetTriple(triple);

    IRBuilder<> builder(*ctx);
    JIT jit(ctx.get(), mod.get(), builder);
    jit.declare_runtime_functions();

    jit.main_info_ = jit.analyze_program(*ast);
    jit.current_info_ = &jit.main_info_;
    jit.any_kw_only_in_program_ = scan_for_kw_only_marker(*ast);

    auto mainFnType = FunctionType::get(builder.getVoidTy(), {}, false);
    auto mainFn = Function::Create(mainFnType, GlobalValue::ExternalLinkage,
                                   "__culebra_main", mod.get());
    if (jit.main_info_.has_eh) {
      mainFn->setPersonalityFn(jit.get_personality_fn());
    }
    auto entryBB = BasicBlock::Create(*ctx, "entry", mainFn);
    builder.SetInsertPoint(entryBB);
    jit.push_scope();
    llvm::Value* mainMark = nullptr;
    if (jit.main_info_.has_fn_defer) {
      mainMark = builder.CreateCall(
          mod->getFunction(rt::defer_mark), {}, "main.mark");
      jit.current_fn_defer_mark_ = mainMark;
    }
    jit.pre_allocate_forward_refs(*ast);
    jit.compile(*ast);
    if (!builder.GetInsertBlock()->getTerminator()) {
      if (mainMark) {
        builder.CreateCall(
            mod->getFunction(rt::defer_run_to), {mainMark});
      }
      jit.release_all_scopes_for_exit();
      builder.CreateRetVoid();
    }
    jit.current_fn_defer_mark_ = nullptr;
    jit.pop_scope();
    jit.current_info_ = nullptr;
    verifyFunction(*mainFn);

    // Emit C entry: `int main(int argc, char** argv)` that
    // tail-calls `culebra_aot_bootstrap(argc, argv, __culebra_main)`.
    auto i32 = builder.getInt32Ty();
    auto ptrTy = PointerType::get(*ctx, 0);
    auto bootstrapTy = FunctionType::get(i32, {i32, ptrTy, ptrTy}, false);
    auto bootstrapFn = Function::Create(
        bootstrapTy, GlobalValue::ExternalLinkage,
        "culebra_aot_bootstrap", mod.get());

    auto cMainTy = FunctionType::get(i32, {i32, ptrTy}, false);
    auto cMain = Function::Create(
        cMainTy, GlobalValue::ExternalLinkage, "main", mod.get());
    auto cEntry = BasicBlock::Create(*ctx, "entry", cMain);
    builder.SetInsertPoint(cEntry);
    auto argIt = cMain->arg_begin();
    llvm::Value* argcArg = &*argIt++;
    llvm::Value* argvArg = &*argIt;
    auto callRet = builder.CreateCall(
        bootstrapFn, {argcArg, argvArg, mainFn});
    builder.CreateRet(callRet);
    verifyFunction(*cMain);

    if (opt_level > 0) {
      optimize_module(*mod, opt_level);
    }

    if (emit_llvm) {
      mod->print(outs(), nullptr);
    }

    std::error_code EC;
    llvm::raw_fd_ostream OS(out_path, EC, llvm::sys::fs::OF_None);
    if (EC) {
      std::fprintf(stderr, "culebra build: cannot open %s: %s\n",
                   out_path.c_str(), EC.message().c_str());
      return 1;
    }
    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, OS, nullptr,
                                llvm::CodeGenFileType::ObjectFile)) {
      std::fprintf(stderr,
                   "culebra build: target does not support object emission\n");
      return 1;
    }
    pm.run(*mod);
    OS.flush();
    return 0;
  }

  // Multi-module variant of build_object. Same target-machine,
  // bootstrap-trampoline, and object emission as the single-AST form;
  // the body section follows run_modules — each dependency compiles
  // into its own scope and registers an export Object before the
  // entry module shares the rest of __culebra_main.
  static inline int build_object(const std::vector<LoadedModule>& orig_modules,
                                 const std::string& out_path,
                                 int opt_level = 2,
                                 bool emit_llvm = false,
                                 const std::string& target_triple = "") {
    using namespace llvm;
    if (orig_modules.empty()) {
      std::fprintf(stderr, "culebra build: empty module list\n");
      return 1;
    }

    // Prepend the built-in trait preamble so AOT binaries also see
    // Stringer / Eq / Comparable (mirrors run_modules).
    std::vector<LoadedModule> modules;
    modules.reserve(orig_modules.size() + 1);
    if (auto pre_ast = culebra::parse_builtin_traits_preamble()) {
      LoadedModule preamble;
      preamble.abs_path = "<builtin>";
      preamble.source = std::make_shared<std::string>(
          std::string(culebra::builtin_traits_preamble()));
      preamble.ast = pre_ast;
      modules.push_back(std::move(preamble));
    }
    for (const auto& m : orig_modules) modules.push_back(m);

    if (target_triple.empty()) {
      ensure_native_target_init();
    } else {
      ensure_all_targets_init();
    }

    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>("culebra", *ctx);

    llvm::Triple triple(target_triple.empty()
                            ? llvm::sys::getDefaultTargetTriple()
                            : target_triple);
    std::string err;
    auto* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) {
      std::fprintf(stderr,
                   "culebra build: target lookup failed for '%s': %s\n",
                   triple.str().c_str(), err.c_str());
      return 1;
    }
    std::unique_ptr<llvm::TargetMachine> tm(
        target->createTargetMachine(triple, "", "", {}, {}));
    if (!tm) {
      std::fprintf(stderr,
                   "culebra build: target machine creation failed\n");
      return 1;
    }
    mod->setDataLayout(tm->createDataLayout());
    mod->setTargetTriple(triple);

    IRBuilder<> builder(*ctx);
    JIT jit(ctx.get(), mod.get(), builder);
    jit.declare_runtime_functions();

    std::vector<FuncInfo> infos;
    infos.reserve(modules.size());
    bool any_eh = false;
    bool any_kw_only = false;
    for (const auto& m : modules) {
      infos.push_back(jit.analyze_program(*m.ast));
      any_eh = any_eh || infos.back().has_eh;
      any_kw_only = any_kw_only || scan_for_kw_only_marker(*m.ast);
    }
    jit.any_kw_only_in_program_ = any_kw_only;

    auto mainFnType = FunctionType::get(builder.getVoidTy(), {}, false);
    auto mainFn = Function::Create(mainFnType, GlobalValue::ExternalLinkage,
                                    "__culebra_main", mod.get());
    if (any_eh) mainFn->setPersonalityFn(jit.get_personality_fn());

    auto entryBB = BasicBlock::Create(*ctx, "entry", mainFn);
    builder.SetInsertPoint(entryBB);

    for (size_t i = 0; i + 1 < modules.size(); ++i) {
      const auto& m = modules[i];
      jit.current_module_path_ = m.abs_path;
      jit.main_info_ = std::move(infos[i]);
      jit.current_info_ = &jit.main_info_;
      jit.push_scope();
      llvm::Value* depMark = nullptr;
      if (jit.main_info_.has_fn_defer) {
        depMark = builder.CreateCall(
            mod->getFunction(rt::defer_mark), {}, "dep.mark");
        jit.current_fn_defer_mark_ = depMark;
      }
      jit.pre_allocate_forward_refs(*m.ast);
      jit.compile(*m.ast);
      jit.emit_build_and_register_export(*m.ast, m.abs_path.string());
      if (depMark) {
        builder.CreateCall(
            mod->getFunction(rt::defer_run_to), {depMark});
      }
      jit.current_fn_defer_mark_ = nullptr;
      jit.pop_scope();
      jit.current_info_ = nullptr;
    }

    const auto& entry = modules.back();
    jit.current_module_path_ = entry.abs_path;
    jit.main_info_ = std::move(infos.back());
    jit.current_info_ = &jit.main_info_;
    jit.push_scope();
    llvm::Value* mainMark = nullptr;
    if (jit.main_info_.has_fn_defer) {
      mainMark = builder.CreateCall(
          mod->getFunction(rt::defer_mark), {}, "main.mark");
      jit.current_fn_defer_mark_ = mainMark;
    }
    jit.pre_allocate_forward_refs(*entry.ast);
    jit.compile(*entry.ast);
    if (!builder.GetInsertBlock()->getTerminator()) {
      if (mainMark) {
        builder.CreateCall(
            mod->getFunction(rt::defer_run_to), {mainMark});
      }
      jit.release_all_scopes_for_exit();
      builder.CreateRetVoid();
    }
    jit.current_fn_defer_mark_ = nullptr;
    jit.pop_scope();
    jit.current_info_ = nullptr;
    verifyFunction(*mainFn);

    auto i32 = builder.getInt32Ty();
    auto ptrTy = PointerType::get(*ctx, 0);
    auto bootstrapTy = FunctionType::get(i32, {i32, ptrTy, ptrTy}, false);
    auto bootstrapFn = Function::Create(
        bootstrapTy, GlobalValue::ExternalLinkage,
        "culebra_aot_bootstrap", mod.get());

    auto cMainTy = FunctionType::get(i32, {i32, ptrTy}, false);
    auto cMain = Function::Create(
        cMainTy, GlobalValue::ExternalLinkage, "main", mod.get());
    auto cEntry = BasicBlock::Create(*ctx, "entry", cMain);
    builder.SetInsertPoint(cEntry);
    auto argIt = cMain->arg_begin();
    llvm::Value* argcArg = &*argIt++;
    llvm::Value* argvArg = &*argIt;
    auto callRet = builder.CreateCall(
        bootstrapFn, {argcArg, argvArg, mainFn});
    builder.CreateRet(callRet);
    verifyFunction(*cMain);

    if (opt_level > 0) {
      optimize_module(*mod, opt_level);
    }
    if (emit_llvm) {
      mod->print(outs(), nullptr);
    }

    std::error_code EC;
    llvm::raw_fd_ostream OS(out_path, EC, llvm::sys::fs::OF_None);
    if (EC) {
      std::fprintf(stderr, "culebra build: cannot open %s: %s\n",
                   out_path.c_str(), EC.message().c_str());
      return 1;
    }
    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, OS, nullptr,
                                llvm::CodeGenFileType::ObjectFile)) {
      std::fprintf(stderr,
                   "culebra build: target does not support object emission\n");
      return 1;
    }
    pm.run(*mod);
    OS.flush();
    return 0;
  }

  // REPL-mode compile-and-run for a single input. State (top-level
  // bindings, user-defined functions, multimethods) persists between
  // calls via `globals` + the per-session `jit_handle`. Each call
  // builds a fresh module named `__repl_input_N` and addIRModules it
  // to the shared `LLJIT`, so prior inputs' compiled code stays live.
  //
  // Each input's module is added under its own `ResourceTracker`,
  // which makes future bounded eviction (e.g. drop the oldest K
  // tracker's symbols when a session has compiled many inputs)
  // mechanically possible without revisiting the addIRModule
  // sites. We don't evict today; sessions are typically short.
  //
  // Returns the value of the input's final expression (or a nil
  // JitValue if the input had no expression result). Caller owns the
  // +1 — typically prints and releases. Throws CulebraError /
  // std::exception on parse-survived failures so the REPL loop can
  // catch and continue.
  static JitValue run_repl(const std::shared_ptr<peg::Ast>& ast,
                           JitReplGlobals& globals,
                           llvm::orc::LLJIT& jit_handle) {
    using namespace llvm;
    ensure_native_target_init();

    static std::atomic<int> input_counter{0};
    int input_n = input_counter.fetch_add(1);
    auto fn_name = std::format("__repl_input_{}", input_n);
    auto mod_name = std::format("repl_{}", input_n);

    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>(mod_name, *ctx);
    {
      llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
      std::string err;
      auto* target = llvm::TargetRegistry::lookupTarget(triple, err);
      if (target) {
        std::unique_ptr<llvm::TargetMachine> tm(
            target->createTargetMachine(triple, "", "", {}, {}));
        if (tm) {
          mod->setDataLayout(tm->createDataLayout());
          mod->setTargetTriple(triple);
        }
      }
    }
    IRBuilder<> builder(*ctx);

    JIT jit(ctx.get(), mod.get(), builder);
    jit.declare_runtime_functions();
    jit.main_info_ = jit.analyze_program(*ast);
    jit.current_info_ = &jit.main_info_;
    jit.any_kw_only_in_program_ = scan_for_kw_only_marker(*ast);
    jit.is_repl_session_ = true;

    // `__repl_input_N() -> Value`. The active globals dict is reached
    // via `_jit_repl_globals_current`, set just before invocation.
    auto fnType = FunctionType::get(jit.valueType_, {}, false);
    auto fn = Function::Create(fnType, GlobalValue::ExternalLinkage,
                               fn_name, mod.get());
    if (jit.main_info_.has_eh) {
      fn->setPersonalityFn(jit.get_personality_fn());
    }

    auto entryBB = BasicBlock::Create(*ctx, "entry", fn);
    builder.SetInsertPoint(entryBB);
    jit.push_scope();
    jit.is_repl_top_level_ = true;

    llvm::Value* mainMark = nullptr;
    if (jit.main_info_.has_fn_defer) {
      mainMark = builder.CreateCall(
          mod->getFunction(rt::defer_mark), {}, "repl.mark");
      jit.current_fn_defer_mark_ = mainMark;
    }

    auto last = jit.compile(*ast);

    if (!builder.GetInsertBlock()->getTerminator()) {
      if (mainMark) {
        builder.CreateCall(mod->getFunction(rt::defer_run_to),
                           {mainMark});
      }
      jit.release_all_scopes_for_exit();
      // `compile` for STATEMENTS returns the last statement's value
      // with a +1. Hand it back to the caller for printing/release.
      if (!last) {
        last = jit.make_nil();
      }
      builder.CreateRet(last);
    }

    jit.current_fn_defer_mark_ = nullptr;
    jit.is_repl_top_level_ = false;
    jit.pop_scope();
    jit.current_info_ = nullptr;

    verifyFunction(*fn);
    // O0 keeps per-input compile latency interactive — the optimizer
    // pipeline (~10-30ms at O2) dominates user-perceived REPL lag,
    // and run-once REPL bodies don't amortize the cost. Functions
    // defined at the prompt are still callable from later inputs;
    // they just won't be inlined/scalar-replaced.
    optimize_module(*mod, 0);

    orc::ThreadSafeContext tsctx(std::move(ctx));
    auto rt = jit_handle.getMainJITDylib().createResourceTracker();
    cantFail(jit_handle.addIRModule(rt,
        orc::ThreadSafeModule(std::move(mod), std::move(tsctx))));

    using ReplFn = JitValue (*)();
    auto reply_fn =
        cantFail(jit_handle.lookup(fn_name)).toPtr<ReplFn>();
    // Scoped activation of the thread-local globals pointer. The
    // dtor restores the previous value on every exit path (normal
    // return, CulebraException rethrow, or generic catch-and-throw),
    // so a future fourth path can't accidentally leak a dangling ptr.
    struct ReplGlobalsScope {
      JitReplGlobals* saved;
      ReplGlobalsScope(JitReplGlobals* g)
          : saved(_jit_repl_globals_current) {
        _jit_repl_globals_current = g;
      }
      ~ReplGlobalsScope() { _jit_repl_globals_current = saved; }
    } scope(&globals);

    try {
      return reply_fn();
    } catch (const CulebraException& e) {
      _culebra_value_release_impl(e.tag, e.data);
      auto s = _culebra_value_to_str_impl(e.tag, e.data);
      try { culebra_runtime_defer_run_to(0); } catch (...) {}
      throw std::runtime_error(std::format("uncaught: {}", s));
    }
  }

  static void optimize_module(llvm::Module& mod, int opt_level) {
    using namespace llvm;
    PassBuilder PB;

    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    OptimizationLevel level;
    switch (opt_level) {
      case 1:
        level = OptimizationLevel::O1;
        break;
      case 2:
        level = OptimizationLevel::O2;
        break;
      case 3:
        level = OptimizationLevel::O3;
        break;
      default:
        level = OptimizationLevel::O2;
        break;
    }

    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(level);
    MPM.run(mod, MAM);
  }

  // Built-in type method dispatch (language-level; lives in jit.h).
  llvm::Value* compile_builtin_method(const std::string& method,
                                      const peg::Ast& argsAst,
                                      llvm::Value* receiver);

  // Higher-order-function inlining: when a HOF (map/filter/reduce/...)
  // is called with a literal lambda or fn expression as the callback,
  // emit the body inline into the iteration loop instead of going
  // through the runtime helper's per-element function-pointer call.
  // Mirrors how Rust monomorphizes iterator adapters and how CPython
  // bytecode-fuses list comprehensions; closes most of the gap to a
  // hand-written `while` loop on the JIT path.
  bool is_inlinable_lambda(const peg::Ast& ast, size_t expected_arity) const;

  template <class PerIter>
  void emit_unary_lambda_body(const peg::Ast& lambda_ast,
                              llvm::Value* elem, PerIter&& per_iter) {
    const auto& info = func_info_.at(&lambda_ast);
    // PARAMETER layout: [MUTABLE, IDENTIFIER, (TYPE_ANNOTATION)?, (DEFAULT)?].
    // Honor `|mut x|` so the inlined HOF body matches compile_fn_common's
    // paramMuts handling and interp's per-param mut flag.
    auto param_name =
        std::string(lambda_ast.nodes[0]->nodes[0]->nodes[1]->token);
    bool param_mut =
        lambda_ast.nodes[0]->nodes[0]->nodes[0]->token == "mut";
    push_scope();
    bool captured = info.captured_locals.contains(param_name);
    define_var(param_name,
               make_var_slot(captured, param_name, elem, param_mut));
    auto result = compile(*lambda_ast.nodes[1]);
    per_iter(elem, result);
    pop_scope();
  }

  template <class StoreResult>
  void emit_binary_lambda_body(const peg::Ast& lambda_ast,
                                llvm::Value* acc_val,
                                llvm::Value* val_val,
                                StoreResult&& store_result) {
    const auto& info = func_info_.at(&lambda_ast);
    auto acc_name =
        std::string(lambda_ast.nodes[0]->nodes[0]->nodes[1]->token);
    bool acc_mut =
        lambda_ast.nodes[0]->nodes[0]->nodes[0]->token == "mut";
    auto val_name =
        std::string(lambda_ast.nodes[0]->nodes[1]->nodes[1]->token);
    bool val_mut =
        lambda_ast.nodes[0]->nodes[1]->nodes[0]->token == "mut";
    push_scope();
    bool acc_captured = info.captured_locals.contains(acc_name);
    define_var(acc_name,
               make_var_slot(acc_captured, acc_name, acc_val, acc_mut));
    bool val_captured = info.captured_locals.contains(val_name);
    define_var(val_name,
               make_var_slot(val_captured, val_name, val_val, val_mut));
    auto result = compile(*lambda_ast.nodes[1]);
    store_result(result);
    pop_scope();
  }

  // Adding a new HOF: implement an `emit_inlined_array_<op>` that
  // builds a fresh state (output Array, accumulator alloca, etc.),
  // calls `emit_array_unary_inline_loop` (or `_binary_acc_inline_loop`
  // for accumulating shapes), and returns the resulting JitValue.
  //
  // PerIter receives the lambda's body result (already +1 owned) and
  // decides what to do: push to output, accumulate, conditional push,
  // discard, etc. It runs inside the loop body's scope — the lambda
  // param's slot has not yet been released, so `elem` is also valid.
  template <class PerIter>
  void emit_array_unary_inline_loop(llvm::Value* arrPtr,
                                    const peg::Ast& lambda_ast,
                                    PerIter&& per_iter);

  llvm::Value* emit_inlined_array_map(llvm::Value* arrPtr,
                                      const peg::Ast& lambda_ast);
  llvm::Value* emit_inlined_array_filter(llvm::Value* arrPtr,
                                          const peg::Ast& lambda_ast);
  llvm::Value* emit_inlined_array_for_each(llvm::Value* arrPtr,
                                            const peg::Ast& lambda_ast);
  llvm::Value* emit_inlined_array_reduce(llvm::Value* arrPtr,
                                          llvm::Value* init,
                                          const peg::Ast& lambda_ast);

  // Iterator-side equivalents. The receiver is an iterator-protocol
  // Object whose `next` property is the per-step closure (its
  // closure pointer is hoisted out of the loop). `iter_val` is the
  // {tag, data} pair already constructed by dispatch_arr_iter's
  // lazy branch.
  template <class PerIter>
  void emit_iter_unary_inline_loop(llvm::Value* iter_val,
                                   const peg::Ast& lambda_ast,
                                   PerIter&& per_iter);
  llvm::Value* emit_inlined_iter_for_each(llvm::Value* iter_val,
                                           const peg::Ast& lambda_ast);
  llvm::Value* emit_inlined_iter_reduce(llvm::Value* iter_val,
                                         llvm::Value* init,
                                         const peg::Ast& lambda_ast);
  // Fused `iter.map(λ).collect()`: walks `iter` with the body of `λ`
  // emitted inline per-element, pushes each result into a fresh Array.
  // Skips the per-element wrapper-iterator allocation that the lazy
  // `iter_map` + `iter_collect` runtime path costs.
  llvm::Value* emit_inlined_iter_map_collect(llvm::Value* iter_val,
                                              const peg::Ast& lambda_ast);

  // Postfix-loop helper: if `ast.nodes[i..]` matches the chain
  // `.map(λ).collect()` with an inlinable λ, return the fused value.
  // Caller must advance its index by 3 (consuming ARGUMENTS,
  // DOT(collect), ARGUMENTS) on a hit. Returns `std::nullopt` when
  // the pattern doesn't match.
  std::optional<llvm::Value*> try_fuse_iter_map_collect(
      const peg::Ast& ast, size_t i, llvm::Value* receiver);

  // Fused `Math.range(N).<HOF>(...)` — emits a direct counter loop
  // (`for i in 0..N`) with the body inlined, skipping the iterator
  // wrapper + capture-cell allocation that `culebra_runtime_math_range`
  // would do per call. The `n_ast` expression is compiled once into a
  // Long bound for the loop.
  template <class PerIter>
  void emit_range_unary_inline_loop(const peg::Ast& n_ast,
                                     const peg::Ast& lambda_ast,
                                     PerIter&& per_iter);
  llvm::Value* emit_inlined_range_for_each(const peg::Ast& n_ast,
                                            const peg::Ast& lambda_ast);
  llvm::Value* emit_inlined_range_map_collect(const peg::Ast& n_ast,
                                               const peg::Ast& lambda_ast);
  llvm::Value* emit_inlined_range_reduce(const peg::Ast& n_ast,
                                          const peg::Ast& init_ast,
                                          const peg::Ast& lambda_ast);

  // Method names with JIT-native codegen in compile_builtin_method.
  // Exposed so a startup self-check (see culebra.h) can guard against
  // drift from the interpreter's builtin tables.
  static const std::unordered_set<std::string_view>& known_builtin_methods() {
    static const std::unordered_set<std::string_view> known = {
        "size",        "push",       "pop",      "reverse", "slice",
        "join",        "index_of",   "contains", "upper",   "lower",
        "trim",        "split",      "starts_with", "ends_with",
        "keys",        "has",        "remove",
        // Array eager + iterator lazy / terminal methods. Tag-dispatched
        // in compile_builtin_method — Array receivers keep the eager
        // path; iterator-protocol Objects route to the lazy runtime.
        "map",         "filter",     "reduce",   "for_each",
        "find",        "any",        "all",      "flat_map", "sort_by",
        "sum",         "product",    "min",      "max",
        "collect",     "count",      "take",     "skip",
        "take_while",  "chain",      "zip",      "enumerate",
        "code_points", "graphemes",  "iter",
        "view",        "split_iter",
        // Tensor methods. (Activations sigmoid/relu/softmax are
        // exposed as `Tensor.sigmoid(t)` namespace functions instead,
        // because they collide with class-method names users
        // commonly define — e.g. microgpt's `Value.relu()`.)
        "shape",       "pow",        "transpose",  "reshape",
        "mean",        "argmax",     "to_array",   "dot",
        "linear_sigmoid", "clone",
        "union",       "intersect",  "diff",
        "sym_diff",    "subset",     "superset",
        "to_array"};
    return known;
  }
  // Extension hooks. Built-in functions and namespaces (Math, IO, Random,
  // Sys, ...) live outside the language core; an embedder installs them
  // by passing a populated ExtensionHooks struct via install_extension()
  // before calling JIT::run(). Any field may be nullptr — coalesced as
  // a no-op at the call site, so the core compiles to a "no extensions"
  // JIT when nothing is registered.
  // Each pointer is zero-initialized via `hooks_{}` below; per-field
  // `= nullptr` would force the compiler to evaluate `JIT&` while JIT
  // itself is still being defined (clang error: default member
  // initializer needed within definition of enclosing class).
  struct ExtensionHooks {
    // Declare runtime function signatures on the LLVM module so emitted
    // calls into the extension link cleanly.
    void (*declare_runtime)(JIT&);
    // CALL with a bare identifier callee (`name(args)`).
    llvm::Value* (*compile_global)(JIT&, const std::string& name,
                                    const peg::Ast& argsAst,
                                    const peg::Ast& callAst);
    // CALL with a `Namespace.method(args)` callee.
    llvm::Value* (*compile_ns_call)(JIT&,
                                     std::string_view ns,
                                     std::string_view method,
                                     const peg::Ast& argsAst,
                                     const peg::Ast& callAst);
    // Bare `Namespace.property` reference (no call following).
    llvm::Value* (*compile_ns_prop)(JIT&,
                                     std::string_view ns,
                                     std::string_view prop);
    // True if `name` is provided by the extension (puts/Math/IO/...).
    // Free-variable analysis uses this to skip names that don't live in
    // user scopes_.
    bool (*is_builtin_var)(const std::string& name);
    // Emit `receiver.method(args)` as a UFCS call into an extension
    // global (`x.puts()` → `puts(x)`). Returns nullptr to fall through
    // to regular method dispatch.
    llvm::Value* (*compile_ufcs_builtin)(JIT&, const std::string& method,
                                          const peg::Ast& argsAst,
                                          llvm::Value* receiver);
  };
  // Inside a RuntimeScope: install into that Runtime (overrides the
  // default, for sandboxing). Outside any scope: install into the
  // process-wide default that all Runtimes fall back to.
  static void install_extension(const ExtensionHooks& hooks) {
    if (culebra::_culebra_current_runtime) {
      culebra::runtime_substate<ExtensionHooks>(culebra::kSlotJitHooks) = hooks;
    } else {
      default_hooks_ = hooks;
    }
  }

  static const ExtensionHooks& current_hooks() {
    auto& rt = culebra::current_runtime();
    if (auto* p =
            static_cast<ExtensionHooks*>(rt.substate[culebra::kSlotJitHooks])) {
      return *p;
    }
    return default_hooks_;
  }

 private:
  static inline ExtensionHooks default_hooks_{};
  // Friend declared so extension implementations (e.g. JitExtension in
  // stdlib_jit.h) can reach JIT internals (builder_/module_/make_long/
  // extract_tag/...) without those being part of the public surface.
  friend struct JitExtension;

  llvm::LLVMContext& ctx_;
  llvm::Module* module_;
  llvm::IRBuilder<>& builder_;
  llvm::StructType* valueType_;    // {i8, i64}
  llvm::StructType* cellType_;     // {Value}
  llvm::StructType* closureType_;  // {ptr fn, i64 n, ptr captures}

  std::vector<Scope> scopes_;

  // Analysis results for each FUNCTION AST node (plus the main program)
  std::map<const peg::Ast*, FuncInfo> func_info_;
  FuncInfo main_info_;

  // When compiling methods of `class Foo<T, U>`, holds {"T", "U"} so
  // param annotations naming a type-param can be rewritten to "Any"
  // (Phase 2 MVP: type-params are documentation, runtime sees Any).
  // Cleared back to empty after the class is done compiling.
  std::vector<std::string_view> class_type_params_;

  // Absolute on-disk path of the module currently being compiled.
  // `compile_import_stmt` resolves its relative path string against
  // this module's directory, mirroring the interp's module_stack_
  // top entry. Set by `run_modules` per module; empty when running
  // through the single-AST `run` path.
  std::filesystem::path current_module_path_;

  // Program-wide pre-scan: true when any FUNCTION/MULTIFN/METHOD AST
  // has a `*` keyword-only separator. When false, every dynamic-callee
  // kw-only runtime guard is dead and the call-site IR can skip the
  // `culebra_runtime_check_pos_count` emit entirely.
  bool any_kw_only_in_program_ = false;

  // REPL codegen flags. The actual globals dict lives in the
  // thread-local `_jit_repl_globals_current`; these two fields only
  // tell codegen *which variant* to emit at each binding/lookup site.
  //
  // `is_repl_session_` — true while compiling any REPL input. Gates
  //   the REPL-aware variants of compile_identifier (read through
  //   `repl_get` from any nesting depth) and of let / multifn at
  //   top-level. CompilerStateSaver does NOT touch this — nested
  //   closure bodies still need to resolve top-level reads.
  //
  // `is_repl_top_level_` — true only at the prompt's top level.
  //   CompilerStateSaver flips it false when codegen descends into a
  //   function body, so let-bindings inside a function become local
  //   cells (not REPL globals). Read sites use the broader
  //   `is_repl_session_` flag instead.
  bool is_repl_session_ = false;
  bool is_repl_top_level_ = false;

  // Currently compiling function's info (to know which locals are cells)
  const FuncInfo* current_info_ = nullptr;

  // LLVM function-level state for the function currently being compiled
  llvm::Value* current_closure_arg_ = nullptr;  // __cls__ argument

  // Current AST position for error reporting
  size_t current_line_ = 0;
  size_t current_column_ = 0;

  // Debug mode flag
  bool debug_enabled_ = false;

  // Current function's declared return type (empty = unchecked). The token is
  // owned by the AST, which outlives the compilation, so string_view is safe.
  std::string_view current_return_type_;

  // Defer-stack mark recorded at the current function's entry. `return`
  // lowers `defer_run_to(this mark)` before emitting `ret` so the
  // function's own defers run regardless of where the return sits.
  llvm::Value* current_fn_defer_mark_ = nullptr;

  // Counter for generating unique function names. Process-wide so
  // names stay unique across separate `JIT::run_repl` calls that
  // share an `LLJIT` instance — a fresh JIT counter would collide
  // with prior REPL inputs' `__culebra_fn_0` symbols.
  static inline std::atomic<int> funcCounter_{0};

  // Counter for per-callsite property-get inline-cache globals.
  int prop_ic_counter_ = 0;

  // Counter for per-callsite property-set inline-cache globals.
  int prop_set_ic_counter_ = 0;

  JIT(llvm::LLVMContext* ctx, llvm::Module* mod, llvm::IRBuilder<>& builder)
      : ctx_(*ctx), module_(mod), builder_(builder) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    valueType_ = llvm::StructType::create(
        ctx_, {builder_.getInt8Ty(), builder_.getInt64Ty()}, "Value");
    // All refcounted types have i32 refcount as field 0.
    cellType_ = llvm::StructType::create(
        ctx_, {builder_.getInt64Ty(), valueType_}, "Cell");
    closureType_ = llvm::StructType::create(
        ctx_,
        {builder_.getInt64Ty(), ptrTy, builder_.getInt64Ty(), ptrTy,
         builder_.getInt64Ty()},
        "Closure");
  }

  // --- Scope management ---

  void push_scope() { scopes_.emplace_back(); }

  // Emit IR to release every owned binding in `scope` and zero the
  // underlying allocas so a subsequent re-entry (loop iteration,
  // recursive call) starts from a clean slate. Borrowed slots — the
  // function's `self` / `this` / params and capture cells — are
  // skipped: their refcounts belong to the caller or the enclosing
  // closure, not to the callee's frame.
  //
  // Releases in reverse declaration order (LIFO) so a later-declared
  // binding is destroyed before an earlier one it may reference.
  void release_scope_slots(const Scope& scope) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    for (auto it = scope.order.rbegin(); it != scope.order.rend(); ++it) {
      const auto& slot = (*it)->second;
      if (!slot.owned) continue;
      if (slot.kind == VarSlot::Stack) {
        auto val = builder_.CreateLoad(valueType_, slot.alloca);
        emit_value_release(val);
        builder_.CreateStore(
            llvm::ConstantAggregateZero::get(valueType_), slot.alloca);
      } else {
        auto cellPtr = builder_.CreateLoad(ptrTy, slot.alloca);
        emit_cell_release(cellPtr);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy),
                             slot.alloca);
      }
    }
  }

  // Release every binding in every active scope. Used at function exit
  // (return / fall-through) so locals' refcounts reach zero in the
  // callee — this is what gives auto-drop interp-equivalent timing.
  // Throw / break / continue paths still leak their inner-scope slots
  // until the cycle collector runs (matches prior JIT behaviour).
  void release_all_scopes_for_exit() {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      release_scope_slots(*it);
    }
  }

  void pop_scope() {
    if (!builder_.GetInsertBlock()->getTerminator()) {
      release_scope_slots(scopes_.back());
    }
    scopes_.pop_back();
  }

  const VarSlot* lookup_var(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->slots.find(name);
      if (found != it->slots.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  void define_var(const std::string& name, VarSlot slot) {
    auto& scope = scopes_.back();
    auto [it, inserted] = scope.slots.try_emplace(name, slot);
    if (inserted) {
      scope.order.push_back(it);
    } else {
      // Re-binding within the same scope keeps the original release
      // position (first-declared wins for LIFO order).
      it->second = slot;
    }
  }

  // Allocate a slot and bind it: picks cell vs stack from the enclosing
  // function's capture set (or stack if no enclosing function). `is_mut`
  // = true for `let mut x = ...`; reassigning a non-mut binding raises
  // ImmutableError downstream.
  void declare_local(const std::string& name, llvm::Value* val,
                     bool is_mut = false) {
    bool captured = current_info_ &&
                    current_info_->captured_locals.contains(name);
    define_var(name, make_var_slot(captured, name, val, is_mut));
  }

  // Forward-reference support: before compiling a scope's statements,
  // pre-allocate Cell slots for every directly-declared name that any
  // nested closure captures. A nested `let g = fn () { f() }` followed
  // by `let f = ...` then resolves `f`'s slot via this pre-allocated
  // cell (initially nil — overwritten by the eventual `let f = ...`
  // assignment). The interp gets this for free via lazy env binding;
  // the JIT must materialise the slot eagerly because closure
  // compilation reads it at IR-emit time.
  //
  // Walks only the direct statements of `statements_ast` — nested
  // blocks (if / while / for-in bodies) introduce their own scopes
  // and are handled by their own compile-time entry points.
  void pre_allocate_forward_refs(const peg::Ast& statements_ast) {
    using namespace peg::udl;
    if (!current_info_) return;
    const auto& captured = current_info_->captured_locals;
    if (captured.empty()) return;
    auto& slots = scopes_.empty()
                      ? scopes_.emplace_back().slots
                      : scopes_.back().slots;
    auto pre = [&](std::string_view name_sv, bool is_mut) {
      std::string name(name_sv);
      if (!captured.contains(name)) return;
      if (slots.find(name) != slots.end()) return;
      auto nil_val = make_nil();
      auto slot = make_cell_slot(name, nil_val, is_mut);
      define_var(name, slot);
    };
    struct DeclInfo { std::string_view name; bool is_mut; };
    auto extract_decl = [](const peg::Ast& node) -> DeclInfo {
      if (node.tag == "MULTIFN_DECL"_ || node.tag == "CLASS_DECL"_) {
        // Skip leading DECORATOR children. multifn / class bindings
        // are not mutable in interp (no `mut` form), so is_mut=false.
        // CLASS_HEAD may carry Generic params (`Box<T>`, `min<T: Bound>`);
        // strip via parse_generic_head — the binding lives under the
        // outer name only.
        size_t i = 0;
        while (i < node.nodes.size() &&
               node.nodes[i]->tag == "DECORATOR"_) i++;
        return {culebra::parse_generic_head(node.nodes[i]->token).outer,
                false};
      }
      if (node.tag == "ASSIGNMENT"_ && !node.nodes.empty() &&
          node.nodes[0]->token == "let") {
        // ASSIGNMENT [LET, MUTABLE, lval-chain..., ASSIGN_OP, EXPRESSION].
        // Only single-name lvalues (lvalcnt == 1) get a name here.
        auto total = static_cast<int>(node.nodes.size());
        auto lvalcnt = total - 4;
        // The slot before ASSIGN_OP may carry a TYPE_ANNOTATION; subtract it.
        if (lvalcnt >= 1 && node.nodes[total - 3]->tag == "TYPE_ANNOTATION"_) {
          lvalcnt--;
        }
        if (lvalcnt == 1) {
          const auto& lval = *node.nodes[2];
          if (lval.tag == "IDENTIFIER"_) {
            // The MUTABLE child carries "mut" (or empty); read it so
            // forward-ref pre-allocation matches the user's intent
            // even when the `let mut x` line is compiled after a
            // closure that captures x.
            bool is_mut = node.nodes[1]->token == "mut";
            return {lval.token, is_mut};
          }
        }
      }
      return {};
    };
    auto handle = [&](const peg::Ast& node) {
      auto d = extract_decl(node);
      if (!d.name.empty()) pre(d.name, d.is_mut);
    };
    if (statements_ast.tag == "STATEMENTS"_) {
      for (auto& node : statements_ast.nodes) handle(*node);
    } else {
      // Single-statement body (e.g. lambda body is an EXPRESSION).
      handle(statements_ast);
    }
  }

  // Raw load (no retain) - for internal ownership transfer
  llvm::Value* load_slot_raw(const VarSlot& slot, const std::string& name) {
    if (slot.kind == VarSlot::Stack) {
      return builder_.CreateLoad(valueType_, slot.alloca, name);
    }
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto cellPtr = builder_.CreateLoad(ptrTy, slot.alloca, name + ".cellp");
    auto valPtr = builder_.CreateStructGEP(cellType_, cellPtr, 1, name + ".vp");
    return builder_.CreateLoad(valueType_, valPtr, name);
  }

  // Load a value from a slot with retain (+1 for caller).
  llvm::Value* load_slot(const VarSlot& slot, const std::string& name) {
    auto val = load_slot_raw(slot, name);
    emit_value_retain(val);
    return val;
  }

  // Store a value into a slot (does NOT retain/release — caller's responsibility).
  void store_slot_raw(const VarSlot& slot, llvm::Value* val) {
    if (slot.kind == VarSlot::Stack) {
      builder_.CreateStore(val, slot.alloca);
      return;
    }
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto cellPtr = builder_.CreateLoad(ptrTy, slot.alloca);
    auto valPtr = builder_.CreateStructGEP(cellType_, cellPtr, 1);
    builder_.CreateStore(val, valPtr);
  }

  // Store with RC semantics: releases previous slot contents (after replacing).
  // Caller's `val` ownership is absorbed into the slot.
  void store_slot(const VarSlot& slot, llvm::Value* val) {
    auto old = load_slot_raw(slot, "old");
    store_slot_raw(slot, val);
    emit_value_release(old);
  }

  // Emit IR to retain a Value (no-op for non-refcounted tags; done in runtime).
  void emit_value_retain(llvm::Value* val) {
    auto tag = extract_tag(val);
    auto data = extract_data(val);
    emit_call(
        module_->getOrInsertFunction(
            rt::value_retain, builder_.getVoidTy(),
            builder_.getInt8Ty(), builder_.getInt64Ty()),
        {tag, data});
  }

  void emit_value_release(llvm::Value* val) {
    auto tag = extract_tag(val);
    auto data = extract_data(val);
    emit_call(
        module_->getOrInsertFunction(
            rt::value_release, builder_.getVoidTy(),
            builder_.getInt8Ty(), builder_.getInt64Ty()),
        {tag, data});
  }

  // IR-side wrapper for `rt::value_swap_owned`.
  void emit_value_swap_owned(llvm::Value* keep, llvm::Value* drop) {
    auto i8Ty = builder_.getInt8Ty();
    auto i64Ty = builder_.getInt64Ty();
    emit_call(
        module_->getOrInsertFunction(
            rt::value_swap_owned, builder_.getVoidTy(),
            i8Ty, i64Ty, i8Ty, i64Ty),
        {extract_tag(drop), extract_data(drop),
         extract_tag(keep), extract_data(keep)});
  }

  void emit_cell_retain(llvm::Value* cellPtr) {
    emit_call(
        module_->getOrInsertFunction(rt::cell_retain,
                                     builder_.getVoidTy(),
                                     llvm::PointerType::get(ctx_, 0)),
        {cellPtr});
  }

  void emit_cell_release(llvm::Value* cellPtr) {
    emit_call(
        module_->getOrInsertFunction(rt::cell_release,
                                     builder_.getVoidTy(),
                                     llvm::PointerType::get(ctx_, 0)),
        {cellPtr});
  }

  llvm::Value* current_line_val() {
    return builder_.getInt64(static_cast<int64_t>(current_line_));
  }
  llvm::Value* current_column_val() {
    return builder_.getInt64(static_cast<int64_t>(current_column_));
  }

  // Persist `val` into the REPL session globals dict at runtime.
  // The +1 ABI on `val` flows into `repl_set` (which takes ownership);
  // callers that still need a usable copy upstream must emit a
  // `value_retain` before calling. `name_hint` only annotates the
  // module-level string constant for IR readability.
  void emit_repl_persist(llvm::Value* val, std::string_view name,
                          const char* name_hint, bool is_let,
                          bool is_mut) {
    auto namePtr = get_or_create_global_str(name, name_hint);
    emit_call(
        module_->getFunction(rt::repl_set),
        {namePtr, extract_tag(val), extract_data(val),
         builder_.getInt8(is_let ? 1 : 0),
         builder_.getInt8(is_mut ? 1 : 0),
         current_line_val(), current_column_val()});
  }

  void emit_type_error() {
    emit_call(
        module_->getOrInsertFunction(rt::type_error,
                                     builder_.getVoidTy(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {current_line_val(), current_column_val()});
  }

  // Declare (or fetch) a runtime helper and stamp `noreturn` on it.
  // Every emit_*_throw goes through here so the optimizer treats the
  // call site as terminating.
  llvm::FunctionCallee declare_noreturn(
      const char* name,
      llvm::ArrayRef<llvm::Type*> args) {
    auto fnTy = llvm::FunctionType::get(builder_.getVoidTy(), args, false);
    auto callee = module_->getOrInsertFunction(name, fnTy);
    if (auto* fn = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
      fn->setDoesNotReturn();
    }
    return callee;
  }

  // Emit a `culebra_runtime_immutable_assign(name, line, col)` call
  // that always throws — `compile_assignment` uses this to halt the
  // current path before storing into a non-mut slot.
  void emit_immutable_assign_throw(const std::string& name,
                                    size_t line, size_t col) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto namePtr = get_or_create_global_str(name, ".imm.name");
    emit_call(
        declare_noreturn(rt::immutable_assign,
                         {ptrTy, builder_.getInt64Ty(),
                          builder_.getInt64Ty()}),
        {namePtr, builder_.getInt64(line), builder_.getInt64(col)});
  }

  // Throw an "unknown keyword argument 'name'" TypeError at runtime,
  // matching interp's catchable behavior. The static kwargs resolver
  // routes through here when it detects a leftover kwarg name; the
  // dynamic resolver already throws from inside a runtime helper.
  void emit_unknown_kwarg_throw(const std::string& name,
                                 size_t line, size_t col) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto namePtr = get_or_create_global_str(name, ".kw.name");
    emit_call(
        declare_noreturn(rt::unknown_kwarg,
                         {ptrTy, builder_.getInt64Ty(),
                          builder_.getInt64Ty()}),
        {namePtr, builder_.getInt64(line), builder_.getInt64(col)});
  }

  // Throw a "missing required argument 'name'" ArityError at runtime.
  // Same rationale as emit_unknown_kwarg_throw.
  void emit_missing_required_arg_throw(const std::string& name,
                                        size_t line, size_t col) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto namePtr = get_or_create_global_str(name, ".arg.name");
    emit_call(
        declare_noreturn(rt::missing_required_arg,
                         {ptrTy, builder_.getInt64Ty(),
                          builder_.getInt64Ty()}),
        {namePtr, builder_.getInt64(line), builder_.getInt64(col)});
  }

  // Generic compile-time-detected error → runtime throw. Used to
  // migrate previously-uncatchable `throw culebra::CulebraError(...)`
  // sites in `compile_*` so `try { ... } catch e { ... }` can observe
  // them the same way interp does.
  void emit_throw_error(const char* kind, const std::string& msg,
                         size_t line, size_t col) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto kindPtr = get_or_create_global_str(kind, ".thr.kind");
    auto msgPtr = get_or_create_global_str(msg, ".thr.msg");
    emit_call(
        declare_noreturn(rt::throw_error,
                         {ptrTy, ptrTy, builder_.getInt64Ty(),
                          builder_.getInt64Ty()}),
        {kindPtr, msgPtr,
         builder_.getInt64(line), builder_.getInt64(col)});
  }

  // Emit a typed type-error throw with "expected X, got Y" context.
  // `expected` is a compile-time string literal; `got_tag` is the i8
  // LLVM value of the actual operand's runtime tag (e.g. extract_tag(v)).
  void emit_type_error_typed(const char* expected, llvm::Value* got_tag) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto exp_str = builder_.CreateGlobalStringPtr(expected);
    emit_call(
        module_->getOrInsertFunction(rt::type_error_typed,
                                     builder_.getVoidTy(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty(),
                                     ptrTy,
                                     builder_.getInt8Ty()),
        {current_line_val(), current_column_val(), exp_str, got_tag});
  }

  void emit_div_zero() {
    emit_call(
        module_->getOrInsertFunction(rt::div_zero,
                                     builder_.getVoidTy(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {current_line_val(), current_column_val()});
  }

  // Emit a divide-by-zero check on `divisor`. On zero, throws via
  // emit_div_zero (terminates the basic block); otherwise leaves the
  // builder positioned at the ok-path BB, ready for SDiv/SRem/FDiv/FRem.
  // The compare is selected from `divisor`'s LLVM type; `label_prefix`
  // is used for both BB IR labels (e.g. "div", "fdiv", "mod").
  void emit_div_zero_guard(llvm::Value* divisor, const char* label_prefix) {
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::Value* isZero;
    if (divisor->getType()->isFloatingPointTy()) {
      auto zero = llvm::ConstantFP::get(divisor->getType(), 0.0);
      isZero = builder_.CreateFCmpOEQ(divisor, zero, "iszero");
    } else {
      auto zero = llvm::ConstantInt::get(divisor->getType(), 0);
      isZero = builder_.CreateICmpEQ(divisor, zero, "iszero");
    }
    auto zeroBB = llvm::BasicBlock::Create(
        ctx_, std::string(label_prefix) + ".zero", fn);
    auto okBB = llvm::BasicBlock::Create(
        ctx_, std::string(label_prefix) + ".ok", fn);
    builder_.CreateCondBr(isZero, zeroBB, okBB);
    builder_.SetInsertPoint(zeroBB);
    emit_div_zero();
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(okBB);
  }

  void emit_type_check(llvm::Value* val, std::string_view expected_type,
                       std::string_view context) {
    if (expected_type.empty() || expected_type == "Any") return;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto tag = extract_tag(val);
    auto data = extract_data(val);
    auto expPtr = get_or_create_global_str(expected_type, ".tycheck.exp");
    auto ctxPtr = get_or_create_global_str(context, ".tycheck.ctx");
    emit_call(
        module_->getOrInsertFunction(
            rt::type_check, builder_.getVoidTy(),
            builder_.getInt8Ty(), builder_.getInt64Ty(), ptrTy, ptrTy,
            builder_.getInt64Ty(), builder_.getInt64Ty()),
        {tag, data, expPtr, ctxPtr, current_line_val(),
         current_column_val()});
  }

  // Cache of compile-time string constants to avoid duplicate globals per
  // emission.
  std::unordered_map<std::string, llvm::Constant*> global_str_cache_;
  llvm::Constant* get_or_create_global_str(std::string_view s,
                                           const char* name_hint) {
    std::string key(s);
    auto it = global_str_cache_.find(key);
    if (it != global_str_cache_.end()) return it->second;
    auto* g = builder_.CreateGlobalString(key, name_hint);
    global_str_cache_.emplace(std::move(key), g);
    return g;
  }

  // Get the raw cell pointer from a Cell slot (for passing to closure).
  llvm::Value* cell_ptr_of(const VarSlot& slot) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    return builder_.CreateLoad(ptrTy, slot.alloca, "cellp");
  }

  // --- Value helpers ---

  // `data` must already be i64; callers zext/bitcast/ptrtoint as needed.
  llvm::Value* make_value(uint8_t tag, llvm::Value* data) {
    llvm::Value* val = llvm::UndefValue::get(valueType_);
    val = builder_.CreateInsertValue(val, builder_.getInt8(tag), {0});
    val = builder_.CreateInsertValue(val, data, {1});
    return val;
  }

  llvm::Value* make_nil() { return make_value(TAG_NIL, builder_.getInt64(0)); }

  llvm::Value* make_bool(llvm::Value* b) {
    return make_value(TAG_BOOL, builder_.CreateZExt(b, builder_.getInt64Ty()));
  }

  llvm::Value* make_long(llvm::Value* l) { return make_value(TAG_LONG, l); }

  // Float is stored bit-cast into the i64 payload. The layout of
  // JitValue ({ i8 tag, i64 data }) is unchanged; only the
  // interpretation of `data` differs when tag is TAG_FLOAT.
  llvm::Value* make_float(llvm::Value* d) {
    return make_value(TAG_FLOAT,
                      builder_.CreateBitCast(d, builder_.getInt64Ty(),
                                             "f.bits"));
  }

  llvm::Value* make_ptr_value(uint8_t tag, llvm::Value* ptr) {
    return make_value(tag,
                      builder_.CreatePtrToInt(ptr, builder_.getInt64Ty()));
  }

  llvm::Value* make_func(llvm::Value* ptr) { return make_ptr_value(TAG_FUNC, ptr); }
  llvm::Value* make_string(llvm::Value* ptr) { return make_ptr_value(TAG_STRING, ptr); }
  llvm::Value* make_stringview(llvm::Value* ptr) { return make_ptr_value(TAG_STRINGVIEW, ptr); }
  llvm::Value* make_array(llvm::Value* ptr) { return make_ptr_value(TAG_ARRAY, ptr); }
  llvm::Value* make_tuple(llvm::Value* ptr) { return make_ptr_value(TAG_TUPLE, ptr); }
  llvm::Value* make_set(llvm::Value* ptr) { return make_ptr_value(TAG_SET, ptr); }
  llvm::Value* make_object(llvm::Value* ptr) { return make_ptr_value(TAG_OBJECT, ptr); }
  llvm::Value* make_tensor(llvm::Value* ptr) { return make_ptr_value(TAG_TENSOR, ptr); }

  llvm::Value* extract_ptr(llvm::Value* v) {
    auto data = extract_data(v);
    return builder_.CreateIntToPtr(data, llvm::PointerType::get(ctx_, 0));
  }

  llvm::Value* extract_tag(llvm::Value* v) {
    return builder_.CreateExtractValue(v, {0}, "tag");
  }

  llvm::Value* extract_data(llvm::Value* v) {
    return builder_.CreateExtractValue(v, {1}, "data");
  }

  // value_to_bool: returns i1
  llvm::Value* value_to_bool(llvm::Value* v) {
    auto tag = extract_tag(v);
    auto data = extract_data(v);

    auto fn = builder_.GetInsertBlock()->getParent();
    auto nilBB = llvm::BasicBlock::Create(ctx_, "tobool.nil", fn);
    auto boolBB = llvm::BasicBlock::Create(ctx_, "tobool.bool", fn);
    auto longBB = llvm::BasicBlock::Create(ctx_, "tobool.long", fn);
    auto floatBB = llvm::BasicBlock::Create(ctx_, "tobool.float", fn);
    auto errorBB = llvm::BasicBlock::Create(ctx_, "tobool.error", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "tobool.merge", fn);

    auto sw = builder_.CreateSwitch(tag, errorBB, 4);
    sw->addCase(builder_.getInt8(TAG_NIL), nilBB);
    sw->addCase(builder_.getInt8(TAG_BOOL), boolBB);
    sw->addCase(builder_.getInt8(TAG_LONG), longBB);
    sw->addCase(builder_.getInt8(TAG_FLOAT), floatBB);

    builder_.SetInsertPoint(nilBB);
    auto nilVal = builder_.getFalse();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(boolBB);
    auto boolVal =
        builder_.CreateICmpNE(data, builder_.getInt64(0), "bool.nz");
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(longBB);
    auto longVal =
        builder_.CreateICmpNE(data, builder_.getInt64(0), "long.nz");
    builder_.CreateBr(mergeBB);

    // Float: 0.0 and -0.0 are false; NaN is true (Python's
    // bool(float('nan'))). FCmpUNE("unordered not-equal") returns
    // true when either operand is NaN *or* d != 0 — exactly the
    // NaN-is-truthy rule.
    builder_.SetInsertPoint(floatBB);
    auto asD = builder_.CreateBitCast(data, builder_.getDoubleTy(), "f.bits");
    auto zeroD = llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0);
    auto floatVal = builder_.CreateFCmpUNE(asD, zeroD, "float.nz");
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errorBB);
    emit_type_error_typed("Bool, Long, or Float", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 4, "tobool");
    phi->addIncoming(nilVal, nilBB);
    phi->addIncoming(boolVal, boolBB);
    phi->addIncoming(longVal, longBB);
    phi->addIncoming(floatVal, floatBB);
    return phi;
  }

  // value_to_long: returns i64, calls type_error if not Long
  llvm::Value* value_to_long(llvm::Value* v) {
    auto tag = extract_tag(v);
    auto data = extract_data(v);

    auto fn = builder_.GetInsertBlock()->getParent();
    auto okBB = llvm::BasicBlock::Create(ctx_, "tolong.ok", fn);
    auto errorBB = llvm::BasicBlock::Create(ctx_, "tolong.error", fn);

    auto isLong = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_LONG));
    builder_.CreateCondBr(isLong, okBB, errorBB);

    builder_.SetInsertPoint(errorBB);
    emit_type_error_typed("Long", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(okBB);
    return data;
  }

  // coerce_to_double: returns f64 from a Long (SIToFP) or Float
  // (bitcast). Any other tag raises `type error`. Used on every
  // Float-aware arithmetic and comparison slow path.
  llvm::Value* coerce_to_double(llvm::Value* v) {
    auto tag = extract_tag(v);
    auto data = extract_data(v);

    auto fn = builder_.GetInsertBlock()->getParent();
    auto longBB = llvm::BasicBlock::Create(ctx_, "todouble.long", fn);
    auto floatBB = llvm::BasicBlock::Create(ctx_, "todouble.float", fn);
    auto errorBB = llvm::BasicBlock::Create(ctx_, "todouble.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "todouble.merge", fn);

    auto sw = builder_.CreateSwitch(tag, errorBB, 2);
    sw->addCase(builder_.getInt8(TAG_LONG), longBB);
    sw->addCase(builder_.getInt8(TAG_FLOAT), floatBB);

    builder_.SetInsertPoint(longBB);
    auto fromLong =
        builder_.CreateSIToFP(data, builder_.getDoubleTy(), "l.sitofp");
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(floatBB);
    auto fromFloat =
        builder_.CreateBitCast(data, builder_.getDoubleTy(), "f.bitcast");
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errorBB);
    emit_type_error_typed("Long or Float", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getDoubleTy(), 2, "num.d");
    phi->addIncoming(fromLong, longBB);
    phi->addIncoming(fromFloat, floatBB);
    return phi;
  }

  // --- Free variable analysis ---

  // `self`/`this` are language-core keywords for the implicit method
  // receiver; `range`/`iota` are core globals (see
  // `try_compile_core_global`); everything else (puts/Math/IO/...) is
  // supplied by the registered extension.
  static bool is_builtin_var(const std::string& name) {
    if (name == "self" || name == "this") return true;
    if (name == "range" || name == "iota") return true;
    auto& h = current_hooks();
    return h.is_builtin_var && h.is_builtin_var(name);
  }

  // The sink-name predicate and the pattern-binding visitor live in
  // interpreter.h as free functions in namespace culebra.

  // Throw if `name` appears in any closure-captured outer scope.
  // outer[0] is the top-level (main) scope; its entries behave like
  // globals and may be shadowed freely. outer[1..] are enclosing
  // function locals whose names would be captured by the current
  // function and thus must not be shadowed. `_` is the sink and is
  // exempt from the shadow rule entirely.
  static void check_shadow_against_captures(
      const std::string& name,
      const std::vector<const std::set<std::string>*>& outer,
      size_t line, size_t column) {
    if (is_sink_name(name)) return;
    for (size_t i = 1; i < outer.size(); i++) {
      if (outer[i]->contains(name)) throw_shadow_error(name, line, column);
    }
  }

  void check_pattern_shadow(
      const peg::Ast& pattern,
      const std::vector<const std::set<std::string>*>& outer) const {
    for_each_pattern_binding(
        pattern, [&](std::string_view name, size_t line, size_t col) {
          check_shadow_against_captures(std::string(name), outer, line, col);
        });
  }

  // Collect names introduced by `let x = ...` or by bare `x = ...` where x is
  // not in any outer scope (auto-local). Does not descend into nested
  // functions.
  void collect_fn_locals(
      const peg::Ast& node, std::set<std::string>& locals,
      const std::vector<const std::set<std::string>*>& outer) const {
    using namespace peg::udl;
    if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_) return;

    if (node.tag == "MATCH"_) {
      // MATCH = [subject, MATCH_ARMS]; MATCH_ARM = [PATTERN, (GUARD)?, EXPR].
      // Register pattern-bound names as locals of the enclosing
      // function so that nested closures capturing them are handled
      // correctly by the free-variable analysis (the bindings are
      // then promoted to cells via `captured_locals`). Same mechanism
      // as TRY's catch binding below.
      for (auto& arm : node.nodes[1]->nodes) {
        check_pattern_shadow(*arm->nodes[0], outer);
        for_each_pattern_binding(
            *arm->nodes[0],
            [&](std::string_view name, size_t, size_t) {
              locals.insert(std::string(name));
            });
      }
      // fall through to normal recursive walk
    }

    if (node.tag == "TRY"_) {
      // TRY = [body_block, catch_ident, catch_body]. The catch binding
      // introduces a new local in the enclosing function; register it
      // so nested closures that capture it see it and get a cell.
      // `try ... catch _ { ... }` is the sink form (drop the value).
      auto& id = *node.nodes[1];
      auto name = std::string(id.token);
      check_shadow_against_captures(name, outer, id.line, id.column);
      if (!is_sink_name(name)) locals.insert(name);
      // fall through to walk the bodies
    }

    if (node.tag == "FOR"_) {
      // FOR = [IDENT(var), EXPRESSION(iterable), BLOCK(body)]. The loop
      // binding is BLOCK-SCOPED (visible only within the body), so we
      // deliberately don't add it to the enclosing function's flat
      // `locals` set — otherwise functions defined OUTSIDE the for
      // body in the same enclosing function would wrongly see the
      // binding in their `outer` and treat references to a same-named
      // identifier as an outer-capture.
      //
      // The shadow check still applies: introducing the binding must
      // not collide with a closure-captured name from an enclosing
      // function. Subtree-local visibility is re-established in
      // visit_for_frees' FOR handler so nested closures inside the
      // body can still capture it correctly.
      auto& id = *node.nodes[0];
      auto name = std::string(id.token);
      check_shadow_against_captures(name, outer, id.line, id.column);
      collect_fn_locals(*node.nodes[1], locals, outer);
      collect_fn_locals(*node.nodes[2], locals, outer);
      return;
    }

    if (node.tag == "ASSIGNMENT"_) {
      auto av = culebra::view_assignment(node);
      if (av.lvalcnt == 1 && !av.compound) {
        auto ident_node = node.nodes[av.lvaloff];
        if (ident_node->tag == "IDENTIFIER"_) {
          auto name = std::string(ident_node->token);
          bool is_declare = av.is_let || av.is_mut;

          if (is_declare) {
            check_shadow_against_captures(name, outer, ident_node->line,
                                          ident_node->column);
            if (!is_sink_name(name)) locals.insert(name);
          } else {
            // Bare assignment: local only if not in outer
            bool in_outer = false;
            for (auto* s : outer) {
              if (s->contains(name)) {
                in_outer = true;
                break;
              }
            }
            if (!in_outer && !is_sink_name(name)) {
              locals.insert(name);
            }
          }
        }
      }
      collect_fn_locals(*node.nodes.back(), locals, outer);
      return;
    }

    if (node.tag == "CLASS_DECL"_) {
      // `class Name { ... }` binds `Name` in the enclosing scope.
      // Method bodies are analyzed separately (visit_for_frees), not
      // as part of the enclosing function's local set. Optional
      // leading DECORATOR children precede the CLASS_HEAD. Strip
      // Generic params so `class Pair<K, V>` binds under `Pair`.
      size_t i = 0;
      while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
        collect_fn_locals(*node.nodes[i], locals, outer);
        i++;
      }
      auto& id = *node.nodes[i];
      auto name = std::string(
          culebra::parse_generic_head(id.token).outer);
      check_shadow_against_captures(name, outer, id.line, id.column);
      locals.insert(name);
      return;
    }

    if (node.tag == "TRAIT_DECL"_) {
      // trait declarations don't bind a name in the value env (they
      // live in culebra::trait_registry()). Default-method bodies are
      // analyzed in visit_for_frees, not here.
      return;
    }

    if (node.tag == "MULTIFN_DECL"_) {
      // `fn name(params) body` binds `name` in the enclosing scope.
      // Body is analyzed separately by visit_for_frees as a nested
      // function. Same shape as CLASS_DECL above; CLASS_HEAD's
      // Generic params (`<T: Bound>`) are stripped from the binding.
      size_t i = 0;
      while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
        collect_fn_locals(*node.nodes[i], locals, outer);
        i++;
      }
      auto& id = *node.nodes[i];
      auto name = std::string(
          culebra::parse_generic_head(id.token).outer);
      check_shadow_against_captures(name, outer, id.line, id.column);
      locals.insert(name);
      return;
    }

    if (node.tag == "IMPORT_STMT"_) {
      // `import name from "path"` binds `name` in the enclosing scope.
      auto& id = *node.nodes[0];
      auto name = std::string(id.token);
      check_shadow_against_captures(name, outer, id.line, id.column);
      locals.insert(name);
      return;
    }

    for (auto& c : node.nodes) {
      collect_fn_locals(*c, locals, outer);
    }
  }

  void visit_for_frees(const peg::Ast& node,
                       const std::set<std::string>& my_locals,
                       std::vector<const std::set<std::string>*>& outer,
                       FuncInfo& info) {
    using namespace peg::udl;

    if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_ ||
        node.tag == "DEFER"_ || node.tag == "MULTIFN_DECL"_) {
      // Analyze nested function / defer / multimethod body; its
      // locals/frees don't leak into the enclosing scope, but the
      // enclosing scope owns any captured vars (cells) that it
      // references. FUNCTION and LAMBDA share analyze_function (same
      // AST shape: [params, body]; LAMBDA just lacks the optional
      // RETURN_TYPE slot). MULTIFN_DECL has [name, params, body] —
      // analyze_multifn picks params/body off nodes[1] and the last
      // child.
      // MULTIFN_DECL may carry leading DECORATOR children whose
      // expressions live in the enclosing scope (not the fn's inner
      // scope) — visit them directly so any free vars they reference
      // surface to `info`.
      if (node.tag == "MULTIFN_DECL"_) {
        for (auto& child : node.nodes) {
          if (child->tag != "DECORATOR"_) break;
          visit_for_frees(*child, my_locals, outer, info);
        }
      }
      outer.push_back(&my_locals);
      FuncInfo nested_info;
      if (node.tag == "DEFER"_) {
        nested_info = analyze_defer(node, outer);
      } else if (node.tag == "MULTIFN_DECL"_) {
        nested_info = analyze_multifn(node, outer);
      } else {
        nested_info = analyze_function(node, outer);
      }
      outer.pop_back();
      for (const auto& fv : nested_info.free_vars) {
        if (my_locals.contains(fv)) {
          info.captured_locals.insert(fv);
        } else {
          if (std::find(info.free_vars.begin(), info.free_vars.end(), fv) ==
              info.free_vars.end()) {
            info.free_vars.push_back(fv);
          }
        }
      }
      return;
    }

    if (node.tag == "TRAIT_DECL"_) {
      // Default-impl bodies are nested functions captured from the
      // enclosing scope. Signature-only methods skip analysis.
      size_t i = 0;
      while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
        visit_for_frees(*node.nodes[i], my_locals, outer, info);
        i++;
      }
      // node.nodes[i] is CLASS_HEAD; methods follow.
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        const auto& method = *node.nodes[j];
        bool has_body = false;
        for (size_t k = 2; k < method.nodes.size(); k++) {
          if (method.nodes[k]->tag == "TRAIT_BODY"_) {
            has_body = true; break;
          }
        }
        if (!has_body) continue;
        outer.push_back(&my_locals);
        auto method_info = analyze_trait_method(method, outer);
        outer.pop_back();
        for (const auto& fv : method_info.free_vars) {
          if (my_locals.contains(fv)) {
            info.captured_locals.insert(fv);
          } else {
            if (std::find(info.free_vars.begin(), info.free_vars.end(),
                          fv) == info.free_vars.end()) {
              info.free_vars.push_back(fv);
            }
          }
        }
      }
      return;
    }

    if (node.tag == "CLASS_DECL"_) {
      // Each METHOD is a nested function (params + body) that captures
      // from the enclosing scope. Propagate their free_vars exactly
      // like the FUNCTION branch above. Leading DECORATOR children
      // live in the enclosing scope.
      size_t i = 0;
      while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
        visit_for_frees(*node.nodes[i], my_locals, outer, info);
        i++;
      }
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        const auto& method = *node.nodes[j];
        auto mv = culebra::view_method(method);
        if (mv.is_field) {
          visit_for_frees(*mv.value, my_locals, outer, info);
          continue;
        }
        outer.push_back(&my_locals);
        auto method_info = analyze_method(method, outer);
        outer.pop_back();
        for (const auto& fv : method_info.free_vars) {
          if (my_locals.contains(fv)) {
            info.captured_locals.insert(fv);
          } else {
            if (std::find(info.free_vars.begin(), info.free_vars.end(),
                          fv) == info.free_vars.end()) {
              info.free_vars.push_back(fv);
            }
          }
        }
      }
      return;
    }

    // DOT[IDENTIFIER] is a property name, not a variable reference.
    // The AST optimizer collapses the single-child rule so node.tag
    // reads as IDENTIFIER; use original_tag and check before the
    // IDENTIFIER handler below.
    if (node.original_tag == "DOT"_) {
      return;
    }

    if (node.tag == "IDENTIFIER"_) {
      auto name = std::string(node.token);
      // `__ARGS__` is auto-bound by the function prologue; flag use so
      // the prologue can skip the Array allocation when nothing reads it.
      if (name == "__ARGS__") info.uses_args = true;
      if (my_locals.contains(name) || is_builtin_var(name)) return;
      // Check if in any outer scope — if so, it's a free var
      bool in_outer = false;
      for (auto* scope : outer) {
        if (scope->contains(name)) {
          in_outer = true;
          break;
        }
      }
      if (in_outer) {
        if (std::find(info.free_vars.begin(), info.free_vars.end(), name) ==
            info.free_vars.end()) {
          info.free_vars.push_back(name);
        }
      }
      // else: unresolved (will error at runtime) — don't add as free
      return;
    }

    if (node.tag == "FOR"_) {
      // FOR = [IDENT(var), EXPRESSION(iterable), BLOCK(body)]. The
      // binding is block-scoped to the body — make it visible only
      // while walking the body (by extending `my_locals` for that
      // subtree) so nested closures inside the body can capture it
      // while closures outside the body don't see it. Also register
      // the binding in `info.captured_locals` if any nested closure
      // references it — that flag is what triggers cell promotion at
      // emit_for_body_iteration time.
      visit_for_frees(*node.nodes[1], my_locals, outer, info);
      auto extended = my_locals;
      auto name = std::string(node.nodes[0]->token);
      extended.insert(name);
      visit_for_frees(*node.nodes[2], extended, outer, info);
      // If the body walk pulled `name` into the enclosing function's
      // free-vars (because a nested closure referenced it), we instead
      // mark it captured here and drop it from the free list — the
      // enclosing function owns it.
      auto it = std::find(info.free_vars.begin(), info.free_vars.end(),
                          name);
      if (it != info.free_vars.end()) {
        info.captured_locals.insert(name);
        info.free_vars.erase(it);
      }
      return;
    }

    if (node.tag == "OBJECT_PROPERTY"_) {
      // `view.value` collapses long form (EXPRESSION) and shorthand
      // (IDENTIFIER read-from-scope) so the walker doesn't branch.
      auto pv = culebra::view_object_property(node);
      visit_for_frees(*pv.value, my_locals, outer, info);
      return;
    }

    if (node.tag == "ASSIGNMENT"_) {
      auto av = culebra::view_assignment(node);
      if (av.lvalcnt == 1) {
        // Simple target: `x = expr` / `let x = expr` / `x += expr`.
        // For compound (`x += expr`), x must already exist — visit it as
        // an identifier so the closure-capture analyzer sees the read.
        auto ident_node = node.nodes[av.lvaloff];
        if (ident_node->tag == "IDENTIFIER"_) {
          auto name = std::string(ident_node->token);
          if ((!av.is_let || av.compound) && !my_locals.contains(name) &&
              !is_builtin_var(name)) {
            visit_for_frees(*ident_node, my_locals, outer, info);
          }
        }
      } else {
        // Complex lvalue: primary + postfixes. TYPE_ANNOTATION and
        // ASSIGN_OP sit between the last lvalue and rhs, so stopping at
        // `lvaloff + lvalcnt` naturally skips them.
        for (int i = 0; i < av.lvalcnt; i++) {
          visit_for_frees(*node.nodes[av.lvaloff + i], my_locals, outer, info);
        }
      }
      visit_for_frees(*av.rhs, my_locals, outer, info);
      return;
    }

    for (auto& c : node.nodes) {
      visit_for_frees(*c, my_locals, outer, info);
    }
  }

  // Walk this function's body populating `info.has_eh`,
  // `info.has_fn_defer`, and `scope_has_defer_`. Returns true iff the
  // subtree contains a DEFER at the caller's own scope level — used so
  // an enclosing LEXICAL_SCOPE can mark itself without rescanning.
  // Recursion stops at nested FUNCTION / DEFER boundaries (those own
  // their own defers, analyzed separately). `at_fn_top` is true only
  // while still at the function's top level (not inside any `{}`).
  bool scan_eh_defer(const peg::Ast& node, bool at_fn_top, FuncInfo& info) {
    using namespace peg::udl;
    if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_) return false;
    if (node.tag == "DEFER"_) {
      if (at_fn_top) {
        info.has_fn_defer = true;
        // Function-level defers run on throw via a cleanup landingpad
        // emitted in compile_fn_common; the IR requires a personality
        // function, gated by `has_eh`.
        info.has_eh = true;
      }
      return true;
    }
    if (node.tag == "TRY"_) {
      info.has_eh = true;
      bool any = false;
      for (auto& c : node.nodes) any |= scan_eh_defer(*c, at_fn_top, info);
      return any;
    }
    if (node.tag == "LEXICAL_SCOPE"_) {
      bool inner = false;
      for (auto& c : node.nodes) inner |= scan_eh_defer(*c, false, info);
      if (inner) {
        scope_has_defer_.insert(&node);
        info.has_eh = true;
      }
      return false;  // this scope absorbs its own defers
    }
    bool any = false;
    for (auto& c : node.nodes) any |= scan_eh_defer(*c, at_fn_top, info);
    return any;
  }

  // Body of the shared analysis for user-defined callable AST nodes
  // (FUNCTION or METHOD). `params_ast` / `body_ast` are supplied
  // explicitly because METHOD puts its IDENTIFIER at index 0, pushing
  // the params / body down by one slot. `info_key` is the AST pointer
  // used to key `func_info_` — callers point at the outer FUNCTION /
  // METHOD node so `compile_*` can recover the analysis result later.
  FuncInfo analyze_fn_common(
      const peg::Ast* info_key,
      const peg::Ast& params_ast,
      const peg::Ast& body_ast,
      std::vector<const std::set<std::string>*>& outer) {
    std::set<std::string> my_locals;
    for (auto& p : params_ast.nodes) {
      if (culebra::is_kw_only_sep(*p)) continue;
      auto [name_sv, line, col] = culebra::extract_param_name_loc(*p);
      auto name = std::string(name_sv);
      check_shadow_against_captures(name, outer, line, col);
      my_locals.insert(name);
    }
    collect_fn_locals(body_ast, my_locals, outer);

    FuncInfo info;
    for (auto& p : params_ast.nodes) {
      if (culebra::is_kw_only_sep(*p) || culebra::is_kwargs_rest(*p)) continue;
      if (auto* def = extract_default_expr(*p)) {
        visit_for_frees(*def, my_locals, outer, info);
      }
    }
    visit_for_frees(body_ast, my_locals, outer, info);
    scan_eh_defer(body_ast, true, info);

    func_info_[info_key] = info;
    return info;
  }

  // Shared by FUNCTION ([PARAMETERS, (RETURN_TYPE)?, BLOCK]) and LAMBDA
  // ([LAMBDA_PARAMS, BODY]) — both have params at index 0, so we only
  // distinguish them when extracting the return type at compile time.
  FuncInfo analyze_function(
      const peg::Ast& fnAst,
      std::vector<const std::set<std::string>*>& outer) {
    return analyze_fn_common(&fnAst, *fnAst.nodes[0], *fnAst.nodes[1], outer);
  }

  // METHOD ast: [IDENTIFIER, PARAMETERS, BLOCK]. Analyzed just like a
  // nested FUNCTION — method-dispatch's implicit `this` is already
  // classified as a builtin by `is_builtin_var`, so no extra setup.
  FuncInfo analyze_method(
      const peg::Ast& methodAst,
      std::vector<const std::set<std::string>*>& outer) {
    auto mv = culebra::view_method(methodAst);
    return analyze_fn_common(&methodAst, *mv.params, **mv.body, outer);
  }

  // TRAIT_METHOD: only default-body methods need analysis (sig-only
  // methods carry no body to walk). `view_trait_method` finds the
  // optional TRAIT_BODY regardless of where it lands relative to the
  // optional RETURN_TYPE sibling.
  FuncInfo analyze_trait_method(
      const peg::Ast& traitMethodAst,
      std::vector<const std::set<std::string>*>& outer) {
    auto tv = culebra::view_trait_method(traitMethodAst);
    if (!tv.body) return {};
    return analyze_fn_common(&traitMethodAst, *tv.params, *tv.body, outer);
  }

  // MULTIFN_DECL ast: [IDENTIFIER, PARAMETERS, [RETURN_TYPE,] BLOCK].
  // Analyzed like a nested FUNCTION — body is the last child.
  FuncInfo analyze_multifn(
      const peg::Ast& multifnAst,
      std::vector<const std::set<std::string>*>& outer) {
    using namespace peg::udl;
    // Skip leading DECORATOR children — params live right after the
    // IDENTIFIER (which is itself right after the decorators).
    size_t name_idx = 0;
    while (name_idx < multifnAst.nodes.size() &&
           multifnAst.nodes[name_idx]->tag == "DECORATOR"_) {
      name_idx++;
    }
    auto paramsIdx = name_idx + 1;
    auto bodyIdx = multifnAst.nodes.size() - 1;
    return analyze_fn_common(&multifnAst, *multifnAst.nodes[paramsIdx],
                             *multifnAst.nodes[bodyIdx], outer);
  }

  // `defer { BODY }` behaves like a 0-parameter nested function that
  // closes over the enclosing scope. Shadow checks are unneeded (no
  // params, no let/mut at the defer line itself).
  FuncInfo analyze_defer(
      const peg::Ast& deferAst,
      std::vector<const std::set<std::string>*>& outer) {
    std::set<std::string> my_locals;
    collect_fn_locals(*deferAst.nodes[0], my_locals, outer);

    FuncInfo info;
    visit_for_frees(*deferAst.nodes[0], my_locals, outer, info);
    scan_eh_defer(*deferAst.nodes[0], true, info);

    func_info_[&deferAst] = info;
    return info;
  }

  // One JIT instance per compilation. `func_info_` and
  // `scope_has_defer_` are keyed by `peg::Ast*` and never cleared —
  // only the `JIT::run`-local instance keeps pointer stability.
  // Reusing an instance across compilations would collide on reused
  // node addresses; the assert below guards future misuse.
  FuncInfo analyze_program(const peg::Ast& programAst) {
    assert(func_info_.empty() && scope_has_defer_.empty() &&
           "JIT reused — analyze_program expects a fresh instance");
    std::vector<const std::set<std::string>*> outer;
    std::set<std::string> my_locals;
    collect_fn_locals(programAst, my_locals, outer);

    FuncInfo info;
    visit_for_frees(programAst, my_locals, outer, info);
    scan_eh_defer(programAst, true, info);
    return info;
  }

  // --- Runtime function declarations ---

  void declare_runtime_functions() {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    module_->getOrInsertFunction(rt::puts, builder_.getVoidTy(),
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::type_error,
                                 builder_.getVoidTy(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::type_error_typed,
                                 builder_.getVoidTy(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(), ptrTy,
                                 builder_.getInt8Ty());
    module_->getOrInsertFunction(rt::class_parameters_walk, valueType_, ptrTy);
    // multifn_register_and_install:
    //   JitClosure* (const char* name, JitClosure* body,
    //                const char** param_types, int64_t n_param_types)
    module_->getOrInsertFunction(rt::multifn_register_and_install,
                                 ptrTy, ptrTy, ptrTy, ptrTy,
                                 builder_.getInt64Ty());
    // User-level throw: stashes tag+data in globals and raises a
    // C++ exception; try/catch landingpads read the globals back.
    module_->getOrInsertFunction(rt::throw_,
                                 builder_.getVoidTy(), builder_.getInt8Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::rethrow,
                                 builder_.getVoidTy());
    // Defer: (push takes tag+data of a closure Value)
    module_->getOrInsertFunction(rt::defer_mark,
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::defer_push,
                                 builder_.getVoidTy(), builder_.getInt8Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::defer_run_to,
                                 builder_.getVoidTy(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::type_check, builder_.getVoidTy(),
        builder_.getInt8Ty(), builder_.getInt64Ty(), ptrTy, ptrTy,
        builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::div_zero,
                                 builder_.getVoidTy(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::debugger_break,
                                 builder_.getVoidTy(), ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::value_to_display, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::str_concat, ptrTy, ptrTy,
                                 ptrTy);
    module_->getOrInsertFunction(rt::str_eq,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_cmp,
                                 builder_.getInt32Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::array_new, ptrTy);
    module_->getOrInsertFunction(rt::array_new_reserved, ptrTy,
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_push,
                                 builder_.getVoidTy(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::array_resize, builder_.getVoidTy(), ptrTy,
        builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_get,
                                 builder_.getVoidTy(), ptrTy,
                                 builder_.getInt64Ty(), ptrTy, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::array_set, builder_.getVoidTy(), ptrTy,
        builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_size,
                                 builder_.getInt64Ty(), ptrTy);
    module_->getOrInsertFunction(
        rt::array_set_or_push, builder_.getVoidTy(), ptrTy,
        builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_slice, ptrTy, ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::object_new, ptrTy);
    module_->getOrInsertFunction(
        rt::object_set, builder_.getVoidTy(), ptrTy, ptrTy,
        builder_.getInt1Ty(), builder_.getInt8Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::object_get,
                                 builder_.getVoidTy(), ptrTy, ptrTy, ptrTy,
                                 ptrTy);
    module_->getOrInsertFunction(rt::object_has,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::object_size,
                                 builder_.getInt64Ty(), ptrTy);
    module_->getOrInsertFunction(rt::cell_new, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::closure_new, ptrTy, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::value_retain,
                                 builder_.getVoidTy(),
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::value_release,
                                 builder_.getVoidTy(),
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::cell_retain,
                                 builder_.getVoidTy(), ptrTy);
    module_->getOrInsertFunction(rt::cell_release,
                                 builder_.getVoidTy(), ptrTy);

    // Built-in type methods.
    module_->getOrInsertFunction(rt::str_size,
                                 builder_.getInt64Ty(), ptrTy);
    module_->getOrInsertFunction(rt::str_upper, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_lower, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_trim, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_split, ptrTy, ptrTy,
                                 ptrTy);
    module_->getOrInsertFunction(rt::str_slice, ptrTy, ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::strlike_view, ptrTy,
                                 builder_.getInt8Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::strlike_slice_view, ptrTy,
                                 builder_.getInt8Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::strlike_to_cstr, ptrTy,
                                 builder_.getInt8Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::str_contains,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_starts_with,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_ends_with,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::array_pop,
                                 builder_.getVoidTy(), ptrTy, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::array_slice2, ptrTy, ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_join, ptrTy, ptrTy,
                                 ptrTy);
    module_->getOrInsertFunction(rt::array_contains,
                                 builder_.getInt1Ty(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_index_of,
                                 builder_.getInt64Ty(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_reverse,
                                 builder_.getVoidTy(), ptrTy);
    module_->getOrInsertFunction(rt::object_keys, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::object_remove,
                                 builder_.getVoidTy(), ptrTy, ptrTy);
    // Higher-order array helpers (§17.2): (arr, fn_tag, fn_data, line, col).
    module_->getOrInsertFunction(rt::array_map, ptrTy, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_filter, ptrTy,
                                 ptrTy, builder_.getInt8Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::array_for_each, builder_.getVoidTy(), ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::array_reduce, builder_.getVoidTy(), ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt8Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty(), builder_.getInt64Ty(),
        ptrTy, ptrTy);
    // find (arr, fn_tag, fn_data, line, col, out_tag, out_data)
    module_->getOrInsertFunction(
        rt::array_find, builder_.getVoidTy(), ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty(), ptrTy, ptrTy);
    // any/all (arr, fn_tag, fn_data, line, col) -> i64 (0/1)
    module_->getOrInsertFunction(rt::array_any,
                                 builder_.getInt64Ty(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_all,
                                 builder_.getInt64Ty(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    // flat_map (arr, fn_tag, fn_data, line, col) -> ptr
    module_->getOrInsertFunction(rt::array_flat_map, ptrTy,
                                 ptrTy, builder_.getInt8Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    // sort_by (arr, fn_tag, fn_data, line, col)
    module_->getOrInsertFunction(
        rt::array_sort_by, builder_.getVoidTy(), ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty());
    // sum/product/min/max: (arr, line, col) -> i64
    module_->getOrInsertFunction(rt::array_sum, builder_.getInt64Ty(),
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_product, builder_.getInt64Ty(),
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_min, builder_.getInt64Ty(),
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_max, builder_.getInt64Ty(),
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty());

    // Core globals: range/iota are language-level integer iterator/array
    // factories (`for i in range(n) {}`). math_range backs the `..`
    // range syntax too, so its runtime decl must live in the core.
    module_->getOrInsertFunction(rt::iota, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::math_range, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::check_pos_count, builder_.getVoidTy(),
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    // REPL top-level binding storage. The active globals dict is
    // resolved via a thread-local, so neither helper takes it as an
    // argument. repl_get uses out-parameter shape to dodge the
    // 16-byte {i8, i64} struct return ABI quirk seen earlier.
    module_->getOrInsertFunction(rt::repl_get, builder_.getVoidTy(),
                                 ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::repl_set, builder_.getVoidTy(),
                                 ptrTy,
                                 builder_.getInt8Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt8Ty(),
                                 builder_.getInt8Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());

    auto& h = current_hooks();
    if (h.declare_runtime) h.declare_runtime(*this);
  }

  // --- Main dispatch ---

  llvm::Value* compile(const peg::Ast& ast) {
    using namespace peg::udl;

    // Track position for runtime error messages
    if (ast.line) current_line_ = ast.line;
    if (ast.column) current_column_ = ast.column;

    switch (ast.tag) {
      case "STATEMENTS"_:
        return compile_statements(ast);
      case "WHILE"_:
        return compile_while(ast);
      case "FOR"_:
        return compile_for(ast);
      case "BREAK"_:
        return compile_break(ast);
      case "CONTINUE"_:
        return compile_continue(ast);
      case "IF"_:
        return compile_if(ast);
      case "MATCH"_:
        return compile_match(ast);
      case "FUNCTION"_:
        return compile_function(ast);
      case "LAMBDA"_:
        return compile_lambda(ast);
      case "CALL"_:
        return compile_call_with_builtins(ast);
      case "LEXICAL_SCOPE"_:
        return compile_lexical_scope(ast);
      case "ASSIGNMENT"_:
        return compile_assignment(ast);
      case "DESTRUCTURE_ASSIGN"_:
        return compile_destructure_assign(ast);
      case "RANGE"_:
        return compile_range(ast);
      case "CLASS_DECL"_:
        return compile_class_decl(ast);
      case "TRAIT_DECL"_:
        return compile_trait_decl(ast);
      case "MULTIFN_DECL"_:
        return compile_multifn_decl(ast);
      case "IMPORT_STMT"_:
        return compile_import_stmt(ast);
      case "EXPORT_STMT"_:
        // No-op at compile site. `compile_module` walks the module's
        // AST after the body and builds the export Object explicitly,
        // so the in-place EXPORT_STMT just falls through.
        return make_nil();
      case "LOGICAL_OR"_:
        return compile_logical_or(ast);
      case "NIL_COALESCE"_:
        return compile_nil_coalesce(ast);
      case "LOGICAL_AND"_:
        return compile_logical_and(ast);
      case "CONDITION"_:
        return compile_condition(ast);
      case "UNARY_PLUS"_:
        return compile_unary_plus(ast);
      case "UNARY_MINUS"_:
        return compile_unary_minus(ast);
      case "UNARY_NOT"_:
        return compile_unary_not(ast);
      case "ADDITIVE"_:
        return compile_additive(ast);
      case "MULTIPLICATIVE"_:
        return compile_multiplicative(ast);
      case "POWER"_:
        return compile_power(ast);
      case "IDENTIFIER"_:
        return compile_identifier(ast);
      case "NUMBER"_:
        return compile_number(ast);
      case "FLOAT"_:
        return compile_float(ast);
      case "BOOLEAN"_:
        return compile_bool(ast);
      case "NIL"_:
        return make_nil();
      case "STRING"_:
        return compile_string(ast);
      case "INTERPOLATED_CONTENT"_:
        return compile_interpolated_content(ast);
      case "INTERPOLATED_STRING"_:
        return compile_interpolated_string(ast);
      case "ARRAY"_:
        return compile_array(ast);
      case "TUPLE"_:
        return compile_tuple(ast);
      case "SET"_:
        return compile_set(ast);
      case "OBJECT"_:
        return compile_object(ast);
      case "RETURN"_:
        return compile_return(ast);
      case "DEBUGGER"_:
        return compile_debugger(ast);
      case "THROW"_:
        return compile_throw(ast);
      case "TRY"_:
        return compile_try(ast);
      case "DEFER"_:
        return compile_defer(ast);
    }

    // Token (e.g., operator string)
    if (ast.is_token) {
      return nullptr;  // handled by parent
    }

    // Single child: recurse
    if (ast.nodes.size() == 1) {
      return compile(*ast.nodes[0]);
    }

    return make_nil();
  }

  // --- Statements ---

  llvm::Value* compile_statements(const peg::Ast& ast) {
    if (ast.is_token) {
      return compile(ast);
    }
    if (ast.nodes.empty()) {
      return make_nil();
    }
    llvm::Value* val = nullptr;
    for (auto& node : ast.nodes) {
      if (builder_.GetInsertBlock()->getTerminator()) break;
      // Release previous statement's result (not used)
      if (val) emit_value_release(val);
      val = compile(*node);
    }
    return val ? val : make_nil();
  }

  // --- Literals ---

  llvm::Value* compile_number(const peg::Ast& ast) {
    auto n = ast.token_to_number<long>();
    return make_long(builder_.getInt64(n));
  }

  llvm::Value* compile_float(const peg::Ast& ast) {
    auto d = ast.token_to_number<double>();
    auto constD = llvm::ConstantFP::get(builder_.getDoubleTy(), d);
    return make_float(constD);
  }

  llvm::Value* compile_bool(const peg::Ast& ast) {
    auto b = (ast.token == "true");
    return make_bool(builder_.getInt1(b));
  }

  llvm::Value* compile_string(const peg::Ast& ast) {
    auto str = std::string(ast.token);
    auto global = builder_.CreateGlobalString(str, ".str");
    return make_string(global);
  }

  llvm::Value* compile_interpolated_content(const peg::Ast& ast) {
    auto str = decode_interpolated_content(ast.token);
    auto global = builder_.CreateGlobalString(str, ".str");
    return make_string(global);
  }

  llvm::Value* compile_array(const peg::Ast& ast) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    // Create empty array
    auto arrPtr = emit_call(
        module_->getOrInsertFunction(rt::array_new, ptrTy),
        {}, "arr");

    // Resize first if size/default specified (matches interpreter order)
    if (ast.nodes.size() >= 2) {
      auto countVal = compile(*ast.nodes[1]);
      auto count = value_to_long(countVal);
      llvm::Value* defTag = builder_.getInt8(TAG_NIL);
      llvm::Value* defData = builder_.getInt64(0);
      if (ast.nodes.size() >= 3) {
        auto defVal = compile(*ast.nodes[2]);
        defTag = extract_tag(defVal);
        defData = extract_data(defVal);
      }
      emit_call(
          module_->getOrInsertFunction(
              rt::array_resize, builder_.getVoidTy(), ptrTy,
              builder_.getInt64Ty(), builder_.getInt8Ty(),
              builder_.getInt64Ty()),
          {arrPtr, count, defTag, defData});
    }

    // Overwrite or push literal values (matches interpreter:
    // literals go into positions 0..n-1, extending if needed)
    const auto& seqNodes = ast.nodes[0]->nodes;
    for (auto i = 0u; i < seqNodes.size(); i++) {
      auto val = compile(*seqNodes[i]);
      auto tag = extract_tag(val);
      auto data = extract_data(val);
      emit_call(
          module_->getOrInsertFunction(
              rt::array_set_or_push, builder_.getVoidTy(),
              ptrTy, builder_.getInt64Ty(), builder_.getInt8Ty(),
              builder_.getInt64Ty()),
          {arrPtr, builder_.getInt64(i), tag, data});
    }

    return make_array(arrPtr);
  }

  // Tuple literal: `(a, b, c)`. Allocate a TAG_TUPLE-tagged JitArray,
  // compile and push each element. AST shape: nodes are the elements
  // directly (the TUPLE rule expands `EXPRESSION (',' EXPRESSION)+`
  // into nested children — peglib flattens them).
  llvm::Value* compile_tuple(const peg::Ast& ast) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto tupPtr = emit_call(
        module_->getOrInsertFunction(rt::tuple_new, ptrTy), {}, "tup");
    for (const auto& node : ast.nodes) {
      auto val = compile(*node);
      auto tag = extract_tag(val);
      auto data = extract_data(val);
      emit_call(
          module_->getOrInsertFunction(
              rt::tuple_push, builder_.getVoidTy(), ptrTy,
              builder_.getInt8Ty(), builder_.getInt64Ty()),
          {tupPtr, tag, data});
    }
    return make_tuple(tupPtr);
  }

  // Set literal: `{a, b, c}`. Compiles each element, hands its +1
  // reference to `set_add` which dedupes against the sidecar index.
  llvm::Value* compile_set(const peg::Ast& ast) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto setPtr = emit_call(
        module_->getOrInsertFunction(rt::set_new, ptrTy), {}, "set");
    for (const auto& node : ast.nodes) {
      auto val = compile(*node);
      auto tag = extract_tag(val);
      auto data = extract_data(val);
      emit_call(
          module_->getOrInsertFunction(
              rt::set_add, builder_.getVoidTy(), ptrTy,
              builder_.getInt8Ty(), builder_.getInt64Ty()),
          {setPtr, tag, data});
    }
    return make_set(setPtr);
  }

  // Object property bind. The runtime well-known check (drop/iter/
  // next must be 0-arg Function) is emitted only when the literal
  // name is one of the three — keeping the hot path for ordinary
  // literal properties free of any name comparison at runtime.
  //
  // Inlines a per-callsite write IC: load `obj->shape` and compare
  // with the cached expected shape. Match → fast helper that knows
  // the offset and result shape from the IC (no `find_slot` linear
  // scan, no `transition_add` lookup). Miss → slow helper that does
  // the full work and refills the IC. See `JitPropSetIC` and
  // `culebra_runtime_object_set_fast` / `_set_ic`.
  void emit_object_set(llvm::Value* objPtr, const std::string& name,
                       bool mut, llvm::Value* tag, llvm::Value* data) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i8Ty = builder_.getInt8Ty();
    auto i64Ty = builder_.getInt64Ty();
    if (is_well_known_prop(name)) {
      auto wkKey = get_or_create_global_str(name, ".wkkey");
      emit_call(
          module_->getOrInsertFunction(rt::check_well_known_prop,
                                       builder_.getVoidTy(), ptrTy,
                                       i8Ty, i64Ty),
          {wkKey, tag, data});
    }
    auto keyPtr = get_or_create_global_str(name, ".key");

    // IC: { void* expected, void* result, i64 offset, i8 prop_mut }.
    // PrivateLinkage so each module owns its own cache cells. Initial
    // expected_shape uses sentinel `(void*)1` (see compile_property_get
    // for the rationale) so the first call always misses to the slow
    // path; nullptr would spuriously match a fresh Object's null shape.
    auto icTy = llvm::StructType::get(ctx_, {ptrTy, ptrTy, i64Ty, i8Ty});
    auto* sentinelPtr = llvm::ConstantExpr::getIntToPtr(
        llvm::ConstantInt::get(i64Ty, 1), ptrTy);
    auto* icInit = llvm::ConstantStruct::get(
        icTy, {sentinelPtr,
               llvm::ConstantPointerNull::get(ptrTy),
               llvm::ConstantInt::get(i64Ty, 0),
               llvm::ConstantInt::get(i8Ty, 0)});
    auto* icGlobal = new llvm::GlobalVariable(
        *module_, icTy, /*isConstant=*/false,
        llvm::GlobalValue::PrivateLinkage, icInit,
        ".prop.set.ic." + std::to_string(prop_set_ic_counter_++));

    auto fn = builder_.GetInsertBlock()->getParent();
    auto fastBB = llvm::BasicBlock::Create(ctx_, "set.fast", fn);
    auto slowBB = llvm::BasicBlock::Create(ctx_, "set.slow", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "set.merge", fn);

    auto shapeFieldPtr = builder_.CreateConstInBoundsGEP1_64(
        i8Ty, objPtr, offsetof(JitObject, shape), "set.shape.fp");
    auto objShape = builder_.CreateLoad(ptrTy, shapeFieldPtr, "set.obj.shape");
    auto icExpectedPtr =
        builder_.CreateStructGEP(icTy, icGlobal, 0, "set.ic.exp.p");
    auto icExpected =
        builder_.CreateLoad(ptrTy, icExpectedPtr, "set.ic.exp");
    auto shapeMatch =
        builder_.CreateICmpEQ(objShape, icExpected, "set.shape.match");
    builder_.CreateCondBr(shapeMatch, fastBB, slowBB);

    builder_.SetInsertPoint(fastBB);
    emit_call(
        module_->getOrInsertFunction(rt::object_set_fast,
                                     builder_.getVoidTy(), ptrTy, ptrTy,
                                     ptrTy, i8Ty, i64Ty, i64Ty, i64Ty),
        {objPtr, keyPtr, icGlobal, tag, data, current_line_val(),
         current_column_val()});
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(slowBB);
    emit_call(
        module_->getOrInsertFunction(
            rt::object_set_ic, builder_.getVoidTy(), ptrTy, ptrTy, ptrTy,
            builder_.getInt1Ty(), i8Ty, i64Ty, i64Ty, i64Ty),
        {objPtr, keyPtr, icGlobal, builder_.getInt1(mut), tag, data,
         current_line_val(), current_column_val()});
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    // `obj->has_drop` is consulted by the destructor to decide whether
    // to look up `drop` and call it. The runtime helpers used to set
    // this field; with the IC split we know `name` at compile time, so
    // emit the store directly and only when needed. After the merge so
    // it doesn't fire if the slow path threw an immutable-property
    // error (the throw skips merge via the landingpad).
    if (name == "drop") {
      auto hasDropPtr = builder_.CreateConstInBoundsGEP1_64(
          i8Ty, objPtr, offsetof(JitObject, has_drop), "set.has_drop.p");
      builder_.CreateStore(builder_.getInt8(1), hasDropPtr);
    }
  }

  llvm::Value* emit_object_has(llvm::Value* objPtr, llvm::Value* keyPtr,
                               const llvm::Twine& name = "has.prop") {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    return emit_call(
        module_->getOrInsertFunction(rt::object_has, builder_.getInt1Ty(),
                                     ptrTy, ptrTy),
        {objPtr, keyPtr}, name);
  }

  // Build a +1 Object containing the kwargs `merged` collected at a
  // call site that targets a `**rest` catch-all. Returns a Value
  // (TAG_OBJECT) suitable to drop into the user-arg slab.
  llvm::Value* emit_kwargs_rest_object(
      const std::map<std::string_view, const peg::Ast*>& kwargs,
      const peg::Ast& argsAst) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto objPtr = emit_call(
        module_->getOrInsertFunction(rt::object_new, ptrTy), {}, "rest");
    for (const auto& [name, val_ast] : kwargs) {
      auto val = compile(*val_ast);
      emit_object_set(objPtr, std::string(name), /*mut=*/false,
                      extract_tag(val), extract_data(val));
    }
    return make_object(objPtr);
  }

  llvm::Value* compile_object(const peg::Ast& ast) {
    using namespace peg::udl;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    auto objPtr = emit_call(
        module_->getOrInsertFunction(rt::object_new, ptrTy),
        {}, "obj");

    // Each child is OBJECT_PROPERTY — `view_object_property` normalizes
    // the long `{k: v}` and shorthand `{x}` forms. Non-IDENTIFIER keys
    // (Long/Float/Bool/Nil/Tuple literals) route through the value-
    // keyed sidecar via `object_set_any`.
    for (auto& prop : ast.nodes) {
      auto pv = culebra::view_object_property(*prop);

      if (pv.key->tag != "IDENTIFIER"_) {
        // Non-IDENTIFIER literal key — emit Value-keyed set.
        auto key = compile(*pv.key);
        auto val = compile(*pv.value);
        emit_call(
            module_->getOrInsertFunction(
                rt::object_set_any, builder_.getVoidTy(), ptrTy,
                builder_.getInt8Ty(), builder_.getInt64Ty(),
                builder_.getInt1Ty(), builder_.getInt8Ty(),
                builder_.getInt64Ty()),
            {objPtr, extract_tag(key), extract_data(key),
             builder_.getInt1(pv.is_mut), extract_tag(val), extract_data(val)});
        continue;
      }

      auto name = std::string(pv.key->token);
      llvm::Value* val;
      if (pv.is_shorthand) {
        auto slot = lookup_var(name);
        if (!slot) {
          // Match interp's eval-time NameError so the same source
          // raises a catchable exception under --jit. The runtime
          // throw fires before this property would be installed.
          emit_throw_error("NameError",
              std::format("undefined variable '{}'...", name),
              pv.key->line, pv.key->column);
          val = make_nil();
        } else {
          val = load_slot(*slot, name);
        }
      } else {
        val = compile(*pv.value);
      }
      emit_object_set(objPtr, name, pv.is_mut,
                      extract_tag(val), extract_data(val));
    }

    return make_object(objPtr);
  }

  llvm::Value* compile_interpolated_string(const peg::Ast& ast) {
    using namespace peg::udl;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    // Start with empty string
    llvm::Value* result = builder_.CreateGlobalString("", ".str.empty");

    for (auto& node : ast.nodes) {
      llvm::Value* piece;
      if (node->tag == "INTERPOLATED_CONTENT"_) {
        // Raw text between expressions; decode escape sequences first
        // (\n \r \t \\ \" \{) so the runtime sees the resolved bytes.
        auto decoded = decode_interpolated_content(node->token);
        piece = builder_.CreateGlobalString(decoded, ".str");
      } else {
        // Expression - evaluate and convert to display string
        auto val = compile(*node);
        auto tag = extract_tag(val);
        auto data = extract_data(val);
        piece = emit_call(
            module_->getOrInsertFunction(rt::value_to_display,
                                         ptrTy, builder_.getInt8Ty(),
                                         builder_.getInt64Ty()),
            {tag, data}, "disp");
      }
      result = emit_call(
          module_->getOrInsertFunction(rt::str_concat, ptrTy,
                                       ptrTy, ptrTy),
          {result, piece}, "concat");
    }

    return make_string(result);
  }

  // --- Identifier ---

  llvm::Value* compile_identifier(const peg::Ast& ast) {
    auto name = std::string(ast.token);
    auto slot = lookup_var(name);
    if (slot) return load_slot(*slot, name);
    if (is_repl_session_) {
      // Resolve through REPL session globals at runtime. The helper
      // consults a thread-local `_jit_repl_globals_current` so nested
      // closure bodies can use the same path as top-level lookups
      // without needing the globals dict threaded through their
      // signature. Out-params dodge the 16-byte aggregate-return ABI.
      auto fn = builder_.GetInsertBlock()->getParent();
      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      auto tagSlot = entryB.CreateAlloca(builder_.getInt8Ty(),
                                         nullptr, "repl.tag");
      auto dataSlot = entryB.CreateAlloca(builder_.getInt64Ty(),
                                          nullptr, "repl.data");
      auto namePtr = get_or_create_global_str(name, ".repl.id");
      emit_call(
          module_->getFunction(rt::repl_get),
          {namePtr, current_line_val(), current_column_val(),
           tagSlot, dataSlot});
      auto tag = builder_.CreateLoad(builder_.getInt8Ty(), tagSlot,
                                     "repl.tag.v");
      auto data = builder_.CreateLoad(builder_.getInt64Ty(), dataSlot,
                                      "repl.data.v");
      llvm::Value* v = llvm::UndefValue::get(valueType_);
      v = builder_.CreateInsertValue(v, tag, {0});
      v = builder_.CreateInsertValue(v, data, {1});
      return v;
    }
    // bare stdlib namespace (e.g. `let m = IO`) — fetch its lazy-built
    // sentinel Object so the value can be passed around like any user
    // Object. `IO.method(...)` still goes through the fast compile_global
    // path; only this slow path runs when the namespace is used as a
    // value.
    if (is_builtin_var(name)) {
      auto ptrTy = llvm::PointerType::get(ctx_, 0);
      auto fn = builder_.GetInsertBlock()->getParent();
      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      auto tagSlot = entryB.CreateAlloca(builder_.getInt8Ty(),
                                          nullptr, "ns.tag");
      auto dataSlot = entryB.CreateAlloca(builder_.getInt64Ty(),
                                           nullptr, "ns.data");
      auto namePtr = get_or_create_global_str(name, ".ns.name");
      emit_call(
          module_->getOrInsertFunction(rt::namespace_get,
                                       builder_.getVoidTy(),
                                       ptrTy, ptrTy, ptrTy),
          {namePtr, tagSlot, dataSlot});
      auto tag = builder_.CreateLoad(builder_.getInt8Ty(), tagSlot,
                                      "ns.tag.v");
      auto data = builder_.CreateLoad(builder_.getInt64Ty(), dataSlot,
                                       "ns.data.v");
      llvm::Value* v = llvm::UndefValue::get(valueType_);
      v = builder_.CreateInsertValue(v, tag, {0});
      v = builder_.CreateInsertValue(v, data, {1});
      return v;
    }
    // Match interp's eval-time NameError so `try { undefined } catch e
    // { ... }` works under --jit too.
    emit_throw_error("NameError",
        std::format("undefined variable '{}'...", name),
        ast.line, ast.column);
    return make_nil();
  }

  // Create an alloca + cell for a new captured variable.
  // Alloca is pre-initialized to null in entry block for safe release on
  // re-execution (e.g., let inside a loop body).
  VarSlot make_cell_slot(const std::string& name, llvm::Value* initValue,
                         bool is_mut = false) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto cellSlotAlloca = entryB.CreateAlloca(ptrTy, nullptr, name);
    entryB.CreateStore(llvm::ConstantPointerNull::get(ptrTy),
                       cellSlotAlloca);

    // At declaration point: release old cell (null on first run), create new.
    auto oldCell = builder_.CreateLoad(ptrTy, cellSlotAlloca, name + ".old");
    emit_cell_release(oldCell);
    auto tag = extract_tag(initValue);
    auto data = extract_data(initValue);
    auto cellPtr = emit_call(
        module_->getOrInsertFunction(rt::cell_new, ptrTy,
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty()),
        {tag, data}, "cell");
    builder_.CreateStore(cellPtr, cellSlotAlloca);
    return VarSlot{VarSlot::Cell, cellSlotAlloca, /*owned=*/true, is_mut};
  }

  VarSlot make_stack_slot(const std::string& name, llvm::Value* initValue,
                          bool is_mut = false) {
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto alloca = entryB.CreateAlloca(valueType_, nullptr, name);
    entryB.CreateStore(llvm::ConstantAggregateZero::get(valueType_), alloca);
    // At declaration point: use store_slot (releases old = nil first run, else
    // previous iteration's value).
    VarSlot slot{VarSlot::Stack, alloca, /*owned=*/true, is_mut};
    store_slot(slot, initValue);
    return slot;
  }

  VarSlot make_var_slot(bool captured, const std::string& name,
                        llvm::Value* initValue, bool is_mut = false) {
    return captured ? make_cell_slot(name, initValue, is_mut)
                    : make_stack_slot(name, initValue, is_mut);
  }

  // --- Assignment ---

  llvm::Value* compile_assignment(const peg::Ast& ast) {
    using namespace peg::udl;
    // ASSIGNMENT layout (see parser.h `view_assignment`).
    auto av = culebra::view_assignment(ast);
    auto lvaloff = av.lvaloff;
    auto lvalcnt = av.lvalcnt;
    auto let = av.is_let;
    auto mut = av.is_mut;
    bool compound = av.compound;
    auto base_op = av.op_base;

    if (compound && (let || mut)) {
      throw culebra::CulebraError("SyntaxError",
          "compound assignment cannot declare a new variable.");
    }

    auto rval = compile(*av.rhs);

    if (!av.type_annotation.empty()) {
      emit_type_check(rval, av.type_annotation, "assignment");
    }

    if (lvalcnt == 1) {
      auto name = std::string(ast.nodes[lvaloff]->token);

      // Sink: `let _ = expr` / `_ = expr`. The RHS still runs (its side
      // effects matter); the value passes through as the assignment's
      // result with its +1 from compile() intact, since no slot absorbs
      // it. Caller (compile_statements) drops the result like any other
      // unused value.
      if (is_sink_name(name)) {
        return rval;
      }

      // REPL top-level binding: persist into the session globals dict
      // instead of building a local alloca. +1 source: `rval` carries
      // a single +1 from `compile(...)`; we retain so the caller's
      // copy survives `repl_set` taking ownership.
      if (is_repl_top_level_ && is_repl_session_ && !compound) {
        emit_value_retain(rval);
        emit_repl_persist(rval, name, ".repl.let", let, mut);
        return rval;
      }

      if (compound) {
        auto slot = lookup_var(name);
        if (!slot) {
          // Match interp's eval-time NameError (interpreter.h:5000)
          // so `try { x += 1 } catch e { e.kind }` is symmetric.
          emit_throw_error("NameError",
              std::format("compound assignment on undefined name '{}'",
                          name),
              ast.line, ast.column);
          // The throw is the terminal effect; emit a dummy nil so
          // surrounding IR stays well-formed (the runtime unwinds
          // before any of it runs).
          return make_nil();
        }
        auto cur = load_slot(*slot, name);
        // In-place fast path is requested for Tensor lhs; the runtime
        // helper falls back to the plain binop otherwise. When it
        // succeeds for Tensor, new_val is the same handle as cur (mutated
        // buffer in place) — in that case interp skips the env->assign
        // check entirely, so the JIT must also skip rebinding (and the
        // mut check that would gate it).
        auto new_val = emit_arith_step(cur, rval, base_op, /*inplace=*/true);
        // Both cur (from load_slot) and rval (from compile) carry a +1
        // that emit_arith_step did not consume. Drop them here so the
        // path is leak-balanced for refcounted operands (Tensor/Object).
        emit_value_release(rval);
        emit_value_release(cur);

        // Detect Tensor in-place: tag == TAG_TENSOR && handle unchanged.
        // Any other combination (Long, String, Float, fresh Tensor) is
        // a logical rebind and must enforce the slot's mut flag.
        auto isTensor = builder_.CreateICmpEQ(
            extract_tag(cur), builder_.getInt8(TAG_TENSOR));
        auto sameData = builder_.CreateICmpEQ(
            extract_data(cur), extract_data(new_val));
        auto isInPlace = builder_.CreateAnd(isTensor, sameData);

        auto fn = builder_.GetInsertBlock()->getParent();
        auto rebindBB =
            llvm::BasicBlock::Create(ctx_, "compound.rebind", fn);
        auto doneBB =
            llvm::BasicBlock::Create(ctx_, "compound.done", fn);
        builder_.CreateCondBr(isInPlace, doneBB, rebindBB);

        builder_.SetInsertPoint(rebindBB);
        if (!slot->mut) {
          emit_immutable_assign_throw(name, ast.line, ast.column);
        }
        store_slot(*slot, new_val);
        emit_value_retain(new_val);
        builder_.CreateBr(doneBB);

        builder_.SetInsertPoint(doneBB);
        return new_val;
      }

      // `mut x = ...` (no `let`) is also a declaration, matching the
      // interp's `declare = let || mut` rule. Implicit `x = ...`
      // first-occurrence is also a declaration.
      bool declare = let || mut;
      // Check if the variable already exists in the current (innermost) scope.
      // If so, reuse the slot (avoid alloca accumulation in loops with `let`).
      if (!scopes_.empty()) {
        auto& slots = scopes_.back().slots;
        auto it = slots.find(name);
        if (it != slots.end()) {
          if (declare) {
            // Forward-ref pre-allocation or loop re-entry: this is the
            // first / re-running binding. Honor the user-declared mut
            // flag from the source.
            it->second.mut = mut;
          } else {
            // Reassignment (`x = expr`) on an existing same-scope slot.
            if (!it->second.mut) {
              emit_immutable_assign_throw(name, ast.line, ast.column);
            }
          }
          store_slot(it->second, rval);
          emit_value_retain(rval);
          return rval;
        }
      }

      if (!declare) {
        auto existing = lookup_var(name);
        if (existing) {
          if (!existing->mut) {
            emit_immutable_assign_throw(name, ast.line, ast.column);
          }
          store_slot(*existing, rval);
          emit_value_retain(rval);
          return rval;
        }
      }

      declare_local(name, rval, /*is_mut=*/mut);
      emit_value_retain(rval);
      // Record direct `let f = fn (...) {...}` bindings so a later
      // `f(x, y: 2)` can resolve kwargs against the FUNCTION AST at
      // compile time. The RHS lives at ast.nodes.back(); its `tag`
      // (post-optimizer) is "FUNCTION" for a function literal.
      {
        using namespace peg::udl;
        const auto& rhs = *ast.nodes.back();
        if (rhs.tag == "FUNCTION"_ && !scopes_.empty()) {
          scopes_.back().fn_asts[name] = &rhs;
        }
      }
      return rval;
    }

    using namespace peg::udl;

    // Complex lvalue (obj.prop, arr[idx])
    llvm::Value* lval = compile(*ast.nodes[lvaloff]);

    // Process intermediate postfixes (all but the last)
    auto end = lvaloff + lvalcnt - 1;
    for (auto i = lvaloff + 1; i < end; i++) {
      const auto& postfix = *ast.nodes[i];
      switch (postfix.original_tag) {
        case "INDEX"_: {
          // Promote borrowed result of INDEX to +1 owned (mirrors the
          // chained-postfix path at compile_postfix); the symmetric
          // release below would otherwise underflow the inner array.
          auto receiver = lval;
          lval = compile_index_access(postfix, lval);
          emit_value_swap_owned(lval, receiver);
          break;
        }
        case "ARGUMENTS"_:
          lval = compile_function_call(postfix, lval);
          break;
        default:
          throw std::runtime_error(
              "complex lvalue path not supported in JIT");
      }
    }

    // Final postfix - do the assignment
    const auto& finalPostfix = *ast.nodes[end];
    switch (finalPostfix.original_tag) {
      case "INDEX"_: {
        auto ptrTy = llvm::PointerType::get(ctx_, 0);
        auto tag = extract_tag(lval);
        auto fn = builder_.GetInsertBlock()->getParent();
        // Dispatch on receiver type. Array + Object support both plain
        // and compound assignment; the Object compound path uses
        // object_get_any (throws KeyError on missing) and routes the
        // updated value back through object_set_any (mut-checked).
        auto arrBB = llvm::BasicBlock::Create(ctx_, "set.arr", fn);
        auto objBB = llvm::BasicBlock::Create(ctx_, "set.obj", fn);
        auto errBB = llvm::BasicBlock::Create(ctx_, "set.err", fn);
        auto mergeBB = llvm::BasicBlock::Create(ctx_, "set.merge", fn);
        auto isArr =
            builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_ARRAY));
        auto chkObjBB =
            llvm::BasicBlock::Create(ctx_, "set.chk_obj", fn);
        builder_.CreateCondBr(isArr, arrBB, chkObjBB);
        builder_.SetInsertPoint(chkObjBB);
        auto isObj =
            builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));
        builder_.CreateCondBr(isObj, objBB, errBB);

        builder_.SetInsertPoint(errBB);
        emit_type_error_typed("Array or Object", tag);
        builder_.CreateUnreachable();

        // Array path
        builder_.SetInsertPoint(arrBB);
        auto arrPtr = builder_.CreateIntToPtr(extract_data(lval), ptrTy);
        auto idxVal = compile(finalPostfix);
        auto idx = value_to_long(idxVal);
        llvm::Value* to_store_arr = rval;
        if (compound) {
          auto outTag = builder_.CreateAlloca(builder_.getInt8Ty(),
                                              nullptr, "cidx.out.tag");
          auto outData = builder_.CreateAlloca(builder_.getInt64Ty(),
                                               nullptr, "cidx.out.data");
          emit_call(
              module_->getOrInsertFunction(
                  rt::array_get, builder_.getVoidTy(), ptrTy,
                  builder_.getInt64Ty(), ptrTy, ptrTy,
                  builder_.getInt64Ty(), builder_.getInt64Ty()),
              {arrPtr, idx, outTag, outData, current_line_val(),
               current_column_val()});
          auto curTag = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
          auto curData = builder_.CreateLoad(builder_.getInt64Ty(), outData);
          llvm::Value* cur = llvm::UndefValue::get(valueType_);
          cur = builder_.CreateInsertValue(cur, curTag, {0});
          cur = builder_.CreateInsertValue(cur, curData, {1});
          // array_get returns a +0 borrow; emit_arith_step does not consume
          // operands; the Tensor in-place path retains lhs itself before
          // returning. So no retain/release is needed on `cur`.
          to_store_arr = emit_arith_step(cur, rval, base_op, /*inplace=*/true);
          emit_value_release(rval);
        }
        emit_call(
            module_->getOrInsertFunction(
                rt::array_set, builder_.getVoidTy(), ptrTy,
                builder_.getInt64Ty(), builder_.getInt8Ty(),
                builder_.getInt64Ty(), builder_.getInt64Ty(),
                builder_.getInt64Ty()),
            {arrPtr, idx, extract_tag(to_store_arr),
             extract_data(to_store_arr), current_line_val(),
             current_column_val()});
        // `to_store_arr`'s +1 (rval directly for plain set, or the
        // arith-step result for compound) is consumed by array_set;
        // re-retain so the merge sees a +1 result.
        emit_value_retain(to_store_arr);
        auto arrEnd = builder_.GetInsertBlock();
        builder_.CreateBr(mergeBB);

        // Object path: see culebra_runtime_object_set_any. Compound
        // form reads the current value first via object_get_any (which
        // throws KeyError on a missing slot, matching interp's
        // `obj.has(key)` precondition) and applies the arith step in
        // place before writing back.
        builder_.SetInsertPoint(objBB);
        auto objPtr = builder_.CreateIntToPtr(extract_data(lval), ptrTy);
        auto keyVal = compile(finalPostfix);
        llvm::Value* to_store_obj = rval;
        if (compound) {
          // object_get_any and object_set_any both consume the caller's
          // +1 to the key. Retain once up front so each call sees a
          // separate +1.
          emit_value_retain(keyVal);
          auto outTag = builder_.CreateAlloca(builder_.getInt8Ty(),
                                              nullptr, "cobj.out.tag");
          auto outData = builder_.CreateAlloca(builder_.getInt64Ty(),
                                               nullptr, "cobj.out.data");
          emit_call(
              module_->getOrInsertFunction(
                  rt::object_get_any, builder_.getVoidTy(), ptrTy,
                  builder_.getInt8Ty(), builder_.getInt64Ty(), ptrTy,
                  ptrTy),
              {objPtr, extract_tag(keyVal), extract_data(keyVal),
               outTag, outData});
          auto curTag = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
          auto curData =
              builder_.CreateLoad(builder_.getInt64Ty(), outData);
          llvm::Value* cur = llvm::UndefValue::get(valueType_);
          cur = builder_.CreateInsertValue(cur, curTag, {0});
          cur = builder_.CreateInsertValue(cur, curData, {1});
          to_store_obj =
              emit_arith_step(cur, rval, base_op, /*inplace=*/true);
          emit_value_release(rval);
          emit_value_release(cur);
        }
        emit_call(
            module_->getOrInsertFunction(
                rt::object_set_any, builder_.getVoidTy(), ptrTy,
                builder_.getInt8Ty(), builder_.getInt64Ty(),
                builder_.getInt1Ty(), builder_.getInt8Ty(),
                builder_.getInt64Ty()),
            {objPtr, extract_tag(keyVal), extract_data(keyVal),
             builder_.getInt1(mut), extract_tag(to_store_obj),
             extract_data(to_store_obj)});
        // object_set_any consumed to_store_obj's +1; re-retain for the
        // merge so callers see a +1 result.
        emit_value_retain(to_store_obj);
        auto objEnd = builder_.GetInsertBlock();
        builder_.CreateBr(mergeBB);

        builder_.SetInsertPoint(mergeBB);
        auto phi = builder_.CreatePHI(valueType_, 2, "set.r");
        phi->addIncoming(to_store_arr, arrEnd);
        phi->addIncoming(to_store_obj, objEnd);
        emit_value_release(lval);
        return phi;
      }
      case "DOT"_: {
        auto ptrTy = llvm::PointerType::get(ctx_, 0);
        auto tag = extract_tag(lval);
        auto isObj =
            builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));
        auto fn = builder_.GetInsertBlock()->getParent();
        auto okBB = llvm::BasicBlock::Create(ctx_, "propset.ok", fn);
        auto errBB = llvm::BasicBlock::Create(ctx_, "propset.err", fn);
        builder_.CreateCondBr(isObj, okBB, errBB);

        builder_.SetInsertPoint(errBB);
        emit_type_error_typed("Object", tag);
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(okBB);
        auto objPtr = builder_.CreateIntToPtr(extract_data(lval), ptrTy);
        auto name = std::string(finalPostfix.token);
        // For compound (`o.x += rhs`), read current → apply op → write back.
        llvm::Value* to_store = rval;
        if (compound) {
          // Pre-check that the property exists. Without this the
          // JIT would read a missing slot as nil and then `emit_arith_step`
          // would throw TypeError ("nil + N"), whereas the interp throws
          // AttributeError with a more specific message. Match interp.
          auto namePtr = get_or_create_global_str(name, ".propname");
          auto has = emit_call(
              module_->getOrInsertFunction(rt::object_has,
                                           builder_.getInt1Ty(),
                                           ptrTy, ptrTy),
              {objPtr, namePtr}, "propset.has");
          auto hasBB = llvm::BasicBlock::Create(ctx_, "propset.has", fn);
          auto missBB = llvm::BasicBlock::Create(ctx_, "propset.miss", fn);
          builder_.CreateCondBr(has, hasBB, missBB);

          builder_.SetInsertPoint(missBB);
          emit_call(
              module_->getOrInsertFunction(rt::compound_missing_property,
                                           builder_.getVoidTy(),
                                           builder_.getInt64Ty(),
                                           builder_.getInt64Ty()),
              {builder_.getInt64(finalPostfix.line),
               builder_.getInt64(finalPostfix.column)});
          builder_.CreateUnreachable();

          builder_.SetInsertPoint(hasBB);
          // compile_property_get returns +0 borrowed (no slot retain),
          // so cur does not need a matching release; only rval does.
          auto cur = compile_property_get(lval, name);
          to_store = emit_arith_step(cur, rval, base_op, /*inplace=*/true);
          emit_value_release(rval);
        }
        emit_object_set(objPtr, name, mut, extract_tag(to_store),
                        extract_data(to_store));
        emit_value_release(lval);  // release the lvalue's ref
        emit_value_retain(to_store);
        return to_store;
      }
      default:
        throw std::runtime_error("invalid lvalue postfix");
    }
  }

  // `a..b` / `a..=b`: lazy integer iterator (same runtime object as
  // Math.range). Inclusive form bumps the end by one at compile time.
  llvm::Value* compile_range(const peg::Ast& ast) {
    auto start = value_to_long(compile(*ast.nodes[0]));
    auto end = value_to_long(compile(*ast.nodes[2]));
    if (ast.nodes[1]->token == "..=") {
      end = builder_.CreateAdd(end, builder_.getInt64(1), "range.incl");
    }
    auto obj = builder_.CreateCall(
        module_->getFunction(rt::math_range),
        {start, end, builder_.getInt64(1),
         current_line_val(), current_column_val()});
    return make_object(obj);
  }

  // DESTRUCTURE_ASSIGN children: [MUTABLE, (OBJECT_PATTERN|ARRAY_PATTERN), EXPRESSION]
  // Reuses the match-pattern emitter. On runtime mismatch, throws via
  // emit_type_error (same channel used by other "shape mismatch" cases).
  llvm::Value* compile_destructure_assign(const peg::Ast& ast) {
    bool is_mut = ast.nodes[0]->token == "mut";
    const auto& pattern = *ast.nodes[1];
    // Stash the DESTRUCTURE_ASSIGN's own location before compiling the
    // rval — `compile(rval)` advances `current_line_ / current_column_`
    // to inside the rval expression, but the structured error should
    // report the statement-level position so it matches the interp
    // path's `ast.line / ast.column`.
    auto stmt_line = builder_.getInt64(ast.line);
    auto stmt_col = builder_.getInt64(ast.column);
    auto rval = compile(*ast.nodes[2]);

    auto matched = emit_pattern(pattern, rval, is_mut);

    auto fn = builder_.GetInsertBlock()->getParent();
    auto failBB = llvm::BasicBlock::Create(ctx_, "destr.fail", fn);
    auto okBB = llvm::BasicBlock::Create(ctx_, "destr.ok", fn);
    builder_.CreateCondBr(matched, okBB, failBB);

    builder_.SetInsertPoint(failBB);
    emit_call(
        module_->getOrInsertFunction(rt::destructure_mismatch,
                                     builder_.getVoidTy(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {stmt_line, stmt_col});
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(okBB);
    return rval;
  }

  // --- Arithmetic ---
  //
  // Float-aware binary arithmetic uses a 3-way dispatch:
  //   1. If both operands are Long, take the inline i64 fast path
  //      (CreateAdd / CreateMul / CreateSDiv / CreateSRem).
  //   2. Otherwise, call a runtime helper that promotes each operand
  //      to double and runs the operation.
  // The branch predictor learns Long-only code paths immediately, so
  // scripts that stay on Long pay essentially no runtime cost.

  // If both operands are TAG_LONG, run `long_path` inline on their i64
  // payloads; otherwise invoke the runtime helper identified by
  // `rt_name`. Kept as a template so the caller's lambda is inlined
  // (no std::function heap allocation per arithmetic AST node).
  //
  // Three branches:
  //   - bothLong  → `long_path(ldata, rdata)` returning a JitValue
  //   - bothNum   → `float_path(lDouble, rDouble)` returning a JitValue
  //                 (typically `make_float(builder_.CreateFAdd(...))`)
  //   - otherwise → `rt_name` runtime call (Object special-method dispatch)
  template <class LongPath, class FloatPath>
  llvm::Value* emit_binop_dispatch(llvm::Value* lhs, llvm::Value* rhs,
                                   const char* rt_name,
                                   LongPath long_path,
                                   FloatPath float_path) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto intBB = llvm::BasicBlock::Create(ctx_, "binop.int", fn);
    auto checkNumBB =
        llvm::BasicBlock::Create(ctx_, "binop.check_num", fn);
    auto floatBB = llvm::BasicBlock::Create(ctx_, "binop.float", fn);
    auto numBB = llvm::BasicBlock::Create(ctx_, "binop.num", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "binop.merge", fn);

    auto ltag = extract_tag(lhs);
    auto rtag = extract_tag(rhs);
    auto ldata = extract_data(lhs);
    auto rdata = extract_data(rhs);
    auto longTag = builder_.getInt8(TAG_LONG);
    auto floatTag = builder_.getInt8(TAG_FLOAT);
    auto lIsLong = builder_.CreateICmpEQ(ltag, longTag);
    auto rIsLong = builder_.CreateICmpEQ(rtag, longTag);
    auto bothLong = builder_.CreateAnd(lIsLong, rIsLong, "both.long");
    builder_.CreateCondBr(bothLong, intBB, checkNumBB);

    builder_.SetInsertPoint(intBB);
    auto intResult = long_path(ldata, rdata);
    auto intEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(checkNumBB);
    auto lIsFloat = builder_.CreateICmpEQ(ltag, floatTag);
    auto rIsFloat = builder_.CreateICmpEQ(rtag, floatTag);
    auto lIsNum = builder_.CreateOr(lIsLong, lIsFloat);
    auto rIsNum = builder_.CreateOr(rIsLong, rIsFloat);
    auto bothNum = builder_.CreateAnd(lIsNum, rIsNum, "both.num");
    builder_.CreateCondBr(bothNum, floatBB, numBB);

    builder_.SetInsertPoint(floatBB);
    auto doubleTy = llvm::Type::getDoubleTy(ctx_);
    auto lFromFloat = builder_.CreateBitCast(ldata, doubleTy, "l.f");
    auto lFromLong = builder_.CreateSIToFP(ldata, doubleTy, "l.l2f");
    auto lDouble = builder_.CreateSelect(lIsLong, lFromLong, lFromFloat,
                                          "l.d");
    auto rFromFloat = builder_.CreateBitCast(rdata, doubleTy, "r.f");
    auto rFromLong = builder_.CreateSIToFP(rdata, doubleTy, "r.l2f");
    auto rDouble = builder_.CreateSelect(rIsLong, rFromLong, rFromFloat,
                                          "r.d");
    auto floatResult = float_path(lDouble, rDouble);
    auto floatEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(numBB);
    auto numResult = emit_call(
        module_->getOrInsertFunction(rt_name, valueType_,
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty()),
        {ltag, ldata, rtag, rdata}, "num.op");
    auto numEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 3, "binop.r");
    phi->addIncoming(intResult, intEndBB);
    phi->addIncoming(floatResult, floatEndBB);
    phi->addIncoming(numResult, numEndBB);
    return phi;
  }

  // Single arith step for compound-assignment lowering. `inplace=true`
  // swaps the runtime helper for the Tensor-aware in-place variant;
  // Long/Float fast paths are unchanged (those are value types, so
  // in-place doesn't apply).
  llvm::Value* emit_arith_step(llvm::Value* lhs, llvm::Value* rhs,
                               std::string_view op, bool inplace = false) {
    if (op == "@") {
      // No in-place matmul (output shape differs from lhs).
      return emit_call(
          module_->getOrInsertFunction(
              rt::num_matmul, valueType_,
              builder_.getInt8Ty(), builder_.getInt64Ty(),
              builder_.getInt8Ty(), builder_.getInt64Ty()),
          {extract_tag(lhs), extract_data(lhs),
           extract_tag(rhs), extract_data(rhs)}, "cmp.matmul");
    }
    if (op == "**") {
      const char* rt_name = inplace ? rt::num_inplace_pow : rt::num_pow;
      return emit_call(
          module_->getOrInsertFunction(
              rt_name, valueType_,
              builder_.getInt8Ty(), builder_.getInt64Ty(),
              builder_.getInt8Ty(), builder_.getInt64Ty()),
          {extract_tag(lhs), extract_data(lhs),
           extract_tag(rhs), extract_data(rhs)}, "cmp.pow");
    }
    char ope = op[0];
    const char* rt_name = nullptr;
    switch (ope) {
      case '+': rt_name = inplace ? rt::num_inplace_add : rt::num_add; break;
      case '-': rt_name = inplace ? rt::num_inplace_sub : rt::num_sub; break;
      case '*': rt_name = inplace ? rt::num_inplace_mul : rt::num_mul; break;
      case '/': rt_name = inplace ? rt::num_inplace_div : rt::num_div; break;
      case '%': rt_name = rt::num_mod; break;  // mod has no Tensor in-place
      default:
        throw std::runtime_error("invalid compound assignment operator");
    }
    return emit_binop_dispatch(
        lhs, rhs, rt_name,
        [&](llvm::Value* ld, llvm::Value* rd) -> llvm::Value* {
          switch (ope) {
            case '+': return make_long(builder_.CreateAdd(ld, rd, "add"));
            case '-': return make_long(builder_.CreateSub(ld, rd, "sub"));
            case '*': return make_long(builder_.CreateMul(ld, rd, "mul"));
            case '/':
              emit_div_zero_guard(rd, "div");
              return make_long(builder_.CreateSDiv(ld, rd, "div"));
            case '%':
              emit_div_zero_guard(rd, "mod");
              return make_long(builder_.CreateSRem(ld, rd, "mod"));
          }
          return nullptr;
        },
        [&](llvm::Value* lD, llvm::Value* rD) -> llvm::Value* {
          switch (ope) {
            case '+': return make_float(builder_.CreateFAdd(lD, rD, "fadd"));
            case '-': return make_float(builder_.CreateFSub(lD, rD, "fsub"));
            case '*': return make_float(builder_.CreateFMul(lD, rD, "fmul"));
            case '/':
              emit_div_zero_guard(rD, "fdiv");
              return make_float(builder_.CreateFDiv(lD, rD, "fdiv"));
            case '%':
              emit_div_zero_guard(rD, "fmod");
              return make_float(builder_.CreateFRem(lD, rD, "fmod"));
          }
          return nullptr;
        });
  }

  llvm::Value* compile_additive(const peg::Ast& ast) {
    auto lhs = compile(*ast.nodes[0]);
    for (auto i = 1u; i < ast.nodes.size(); i += 2) {
      auto rhs = compile(*ast.nodes[i + 1]);
      auto ope = ast.nodes[i]->token[0];
      const char* rt_name = (ope == '+') ? rt::num_add
                           : (ope == '-') ? rt::num_sub
                                          : nullptr;
      if (!rt_name) throw std::runtime_error("invalid additive operator");
      lhs = emit_binop_dispatch(
          lhs, rhs, rt_name,
          [&](llvm::Value* ld, llvm::Value* rd) -> llvm::Value* {
            auto r = (ope == '+') ? builder_.CreateAdd(ld, rd, "add")
                                  : builder_.CreateSub(ld, rd, "sub");
            return make_long(r);
          },
          [&](llvm::Value* lD, llvm::Value* rD) -> llvm::Value* {
            auto r = (ope == '+') ? builder_.CreateFAdd(lD, rD, "fadd")
                                  : builder_.CreateFSub(lD, rD, "fsub");
            return make_float(r);
          });
    }
    return lhs;
  }

  llvm::Value* compile_multiplicative(const peg::Ast& ast) {
    auto lhs = compile(*ast.nodes[0]);
    for (auto i = 1u; i < ast.nodes.size(); i += 2) {
      auto rhs = compile(*ast.nodes[i + 1]);
      auto ope = ast.nodes[i]->token[0];

      // `@` always goes through the runtime helper — no Long fast path
      // because it has no built-in numeric meaning (dispatch only via
      // `__matmul__`). Emit directly, then skip the dispatch below.
      if (ope == '@') {
        auto ptrTy = llvm::PointerType::get(ctx_, 0);
        (void)ptrTy;
        lhs = emit_call(
            module_->getOrInsertFunction(
                rt::num_matmul, valueType_,
                builder_.getInt8Ty(), builder_.getInt64Ty(),
                builder_.getInt8Ty(), builder_.getInt64Ty()),
            {extract_tag(lhs), extract_data(lhs),
             extract_tag(rhs), extract_data(rhs)}, "matmul");
        continue;
      }

      const char* rt_name = nullptr;
      switch (ope) {
        case '*': rt_name = rt::num_mul; break;
        case '/': rt_name = rt::num_div; break;
        case '%': rt_name = rt::num_mod; break;
        default:
          throw std::runtime_error("invalid multiplicative operator");
      }

      lhs = emit_binop_dispatch(
          lhs, rhs, rt_name,
          [&](llvm::Value* ld, llvm::Value* rd) -> llvm::Value* {
            if (ope == '*') {
              return make_long(builder_.CreateMul(ld, rd, "mul"));
            }
            emit_div_zero_guard(rd, ope == '/' ? "div" : "mod");
            auto r = (ope == '/') ? builder_.CreateSDiv(ld, rd, "div")
                                  : builder_.CreateSRem(ld, rd, "mod");
            return make_long(r);
          },
          [&](llvm::Value* lD, llvm::Value* rD) -> llvm::Value* {
            if (ope == '*') {
              return make_float(builder_.CreateFMul(lD, rD, "fmul"));
            }
            emit_div_zero_guard(rD, ope == '/' ? "fdiv" : "fmod");
            auto r = (ope == '/') ? builder_.CreateFDiv(lD, rD, "fdiv")
                                  : builder_.CreateFRem(lD, rD, "fmod");
            return make_float(r);
          });
    }
    return lhs;
  }

  // --- Unary ---

  llvm::Value* compile_unary_plus(const peg::Ast& ast) {
    return compile(*ast.nodes[1]);
  }

  llvm::Value* compile_unary_minus(const peg::Ast& ast) {
    auto val = compile(*ast.nodes[1]);
    auto tag = extract_tag(val);
    auto longTag = builder_.getInt8(TAG_LONG);
    auto isLong = builder_.CreateICmpEQ(tag, longTag);

    auto fn = builder_.GetInsertBlock()->getParent();
    auto intBB = llvm::BasicBlock::Create(ctx_, "neg.int", fn);
    auto slowBB = llvm::BasicBlock::Create(ctx_, "neg.slow", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "neg.merge", fn);
    builder_.CreateCondBr(isLong, intBB, slowBB);

    builder_.SetInsertPoint(intBB);
    auto intResult = make_long(builder_.CreateNeg(extract_data(val), "neg"));
    auto intEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(slowBB);
    auto slowResult = emit_call(
        module_->getOrInsertFunction(rt::num_neg, valueType_,
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty()),
        {extract_tag(val), extract_data(val)}, "neg.num");
    auto slowEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 2, "neg.r");
    phi->addIncoming(intResult, intEndBB);
    phi->addIncoming(slowResult, slowEndBB);
    return phi;
  }

  llvm::Value* compile_unary_not(const peg::Ast& ast) {
    auto val = compile(*ast.nodes[1]);
    auto b = value_to_bool(val);
    auto notb = builder_.CreateNot(b, "not");
    return make_bool(notb);
  }

  // --- Exponentiation ---

  // If `ast` is a compile-time numeric literal (possibly wrapped in a
  // single unary minus), extract its value. Returns nullopt otherwise.
  struct NumLit { bool is_float; double d; long l; };
  std::optional<NumLit> try_numeric_literal(const peg::Ast& ast) {
    using namespace peg::udl;
    if (ast.original_tag == "UNARY_MINUS"_ && ast.nodes.size() == 2) {
      auto inner = try_numeric_literal(*ast.nodes[1]);
      if (!inner) return std::nullopt;
      if (inner->is_float) return NumLit{true, -inner->d, 0};
      return NumLit{false, 0.0, -inner->l};
    }
    if (ast.tag == "NUMBER"_) {
      return NumLit{false, 0.0, ast.token_to_number<long>()};
    }
    if (ast.tag == "FLOAT"_) {
      return NumLit{true, ast.token_to_number<double>(), 0};
    }
    return std::nullopt;
  }

  llvm::Value* compile_power(const peg::Ast& ast) {
    // POWER = [base, POWER_OPERATOR, exponent]; optimizer strips the
    // node entirely when there's no `**`.
    auto base = compile(*ast.nodes[0]);

    // Peephole specialization for compile-time-constant exponents.
    // Correctness note: sqrt(x) and std::pow(x, 0.5) agree on NaN
    // behavior for negative x, so the 0.5 case is safe to inline.
    if (auto lit = try_numeric_literal(*ast.nodes[2])) {
      if (!lit->is_float && lit->l == 2) {
        return emit_binop_dispatch(
            base, base, rt::num_mul,
            [&](llvm::Value* ld, llvm::Value* rd) {
              return make_long(builder_.CreateMul(ld, rd, "mul"));
            },
            [&](llvm::Value* lD, llvm::Value* rD) {
              return make_float(builder_.CreateFMul(lD, rD, "fmul"));
            });
      }
      if (lit->is_float && lit->d == 0.5) {
        auto d = coerce_to_double(base);
        auto sqrtFn = llvm::Intrinsic::getOrInsertDeclaration(
            module_, llvm::Intrinsic::sqrt, {builder_.getDoubleTy()});
        auto result = builder_.CreateCall(sqrtFn, {d}, "sqrt");
        return make_float(result);
      }
      if ((!lit->is_float && lit->l == 1) ||
          (lit->is_float && lit->d == 1.0)) {
        return base;
      }
    }

    auto exp = compile(*ast.nodes[2]);
    return emit_call(
        module_->getOrInsertFunction(rt::num_pow, valueType_,
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty()),
        {extract_tag(base), extract_data(base),
         extract_tag(exp), extract_data(exp)}, "pow.r");
  }

  // --- Comparison ---

  // Fast Long-Long equality / inequality fast path plus a slow path
  // that forwards to the runtime helper. String, reference-type, and
  // numeric cross-type (Long↔Float) all go through the runtime — it
  // already implements matching semantics with the interpreter.
  llvm::Value* compile_condition(const peg::Ast& ast) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto lhs = compile(*ast.nodes[0]);
    auto ope_str = std::string(ast.nodes[1]->token);
    auto rhs = compile(*ast.nodes[2]);

    auto ltag = extract_tag(lhs);
    auto ldata = extract_data(lhs);
    auto rtag = extract_tag(rhs);
    auto rdata = extract_data(rhs);

    auto fn = builder_.GetInsertBlock()->getParent();

    if (ope_str == "==" || ope_str == "!=") {
      // Fast path: both TAG_LONG — inline icmp. Anything else goes
      // through the runtime equality helper, which handles Float,
      // cross-type numeric (`1 == 1.0`), String by contents, and
      // reference-type identity.
      auto longTag = builder_.getInt8(TAG_LONG);
      auto lIsLong = builder_.CreateICmpEQ(ltag, longTag);
      auto rIsLong = builder_.CreateICmpEQ(rtag, longTag);
      auto bothLong = builder_.CreateAnd(lIsLong, rIsLong, "eq.bothlong");

      auto fastBB = llvm::BasicBlock::Create(ctx_, "eq.fast", fn);
      auto slowBB = llvm::BasicBlock::Create(ctx_, "eq.slow", fn);
      auto mergeBB = llvm::BasicBlock::Create(ctx_, "eq.merge", fn);
      builder_.CreateCondBr(bothLong, fastBB, slowBB);

      builder_.SetInsertPoint(fastBB);
      auto fastEq = builder_.CreateICmpEQ(ldata, rdata, "long.eq");
      auto fastEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(slowBB);
      auto slowEq = emit_call(
          module_->getOrInsertFunction(rt::value_equal,
                                       builder_.getInt1Ty(),
                                       builder_.getInt8Ty(),
                                       builder_.getInt64Ty(),
                                       builder_.getInt8Ty(),
                                       builder_.getInt64Ty()),
          {ltag, ldata, rtag, rdata}, "val.eq");
      auto slowEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(mergeBB);
      auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, "eq.r");
      phi->addIncoming(fastEq, fastEndBB);
      phi->addIncoming(slowEq, slowEndBB);
      llvm::Value* result = phi;
      if (ope_str == "!=") result = builder_.CreateNot(result, "neq");
      return make_bool(result);
    }

    // Ordering operators. Fast path for both-Long; runtime helper
    // handles Float, Long↔Float promotion, String, Nil, and type
    // errors on reference types.
    auto longTag = builder_.getInt8(TAG_LONG);
    auto lIsLong = builder_.CreateICmpEQ(ltag, longTag);
    auto rIsLong = builder_.CreateICmpEQ(rtag, longTag);
    auto bothLong = builder_.CreateAnd(lIsLong, rIsLong, "ord.bothlong");

    auto fastBB = llvm::BasicBlock::Create(ctx_, "ord.fast", fn);
    auto slowBB = llvm::BasicBlock::Create(ctx_, "ord.slow", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "ord.merge", fn);
    builder_.CreateCondBr(bothLong, fastBB, slowBB);

    builder_.SetInsertPoint(fastBB);
    llvm::Value* fastResult;
    if (ope_str == "<") {
      fastResult = builder_.CreateICmpSLT(ldata, rdata);
    } else if (ope_str == "<=") {
      fastResult = builder_.CreateICmpSLE(ldata, rdata);
    } else if (ope_str == ">") {
      fastResult = builder_.CreateICmpSGT(ldata, rdata);
    } else if (ope_str == ">=") {
      fastResult = builder_.CreateICmpSGE(ldata, rdata);
    } else {
      throw std::runtime_error("invalid comparison operator");
    }
    auto fastEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    // Slow path: dispatch to the helper that mirrors the interpreter's
    // Value::operator<{,=,>,=}. Separate helpers (rather than deriving
    // <=, >, >= from <) are necessary because `nil op nil` is `false`
    // for all ordering operators — not the standard total-order
    // relationship.
    builder_.SetInsertPoint(slowBB);
    const char* ord_rt = nullptr;
    if (ope_str == "<") ord_rt = rt::value_less;
    else if (ope_str == "<=") ord_rt = rt::value_leq;
    else if (ope_str == ">") ord_rt = rt::value_greater;
    else if (ope_str == ">=") ord_rt = rt::value_geq;
    else throw std::runtime_error("invalid comparison operator");

    auto slowResult = emit_call(
        module_->getOrInsertFunction(ord_rt,
                                     builder_.getInt1Ty(),
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty()),
        {ltag, ldata, rtag, rdata}, "val.ord");
    auto slowEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, "ord.r");
    phi->addIncoming(fastResult, fastEndBB);
    phi->addIncoming(slowResult, slowEndBB);
    return make_bool(phi);
  }

  // --- Logical operators (short-circuit) ---

  // Left-to-right "keep the first operand satisfying `keep_if`, else
  // fall through to the last" — the shared skeleton for `||` and `??`
  // (logical-and inverts the branch so doesn't share). `label` prefixes
  // generated block/slot names for readable IR.
  template <class KeepPred>
  llvm::Value* compile_short_circuit(const peg::Ast& ast, const char* label,
                                     KeepPred keep_if) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto mergeBB = llvm::BasicBlock::Create(
        ctx_, std::string(label) + ".merge");

    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(),
                                   fn->getEntryBlock().begin());
    auto resultAlloca = entryBuilder.CreateAlloca(
        valueType_, nullptr, std::string(label) + ".tmp");

    for (auto i = 0u; i < ast.nodes.size(); i++) {
      auto val = compile(*ast.nodes[i]);
      builder_.CreateStore(val, resultAlloca);

      if (i < ast.nodes.size() - 1) {
        auto nextBB = llvm::BasicBlock::Create(
            ctx_, std::string(label) + ".next", fn);
        builder_.CreateCondBr(keep_if(val), mergeBB, nextBB);
        builder_.SetInsertPoint(nextBB);
      } else {
        builder_.CreateBr(mergeBB);
      }
    }

    fn->insert(fn->end(), mergeBB);
    builder_.SetInsertPoint(mergeBB);
    return builder_.CreateLoad(
        valueType_, resultAlloca, std::string(label) + ".result");
  }

  llvm::Value* compile_nil_coalesce(const peg::Ast& ast) {
    return compile_short_circuit(
        ast, "coal", [&](llvm::Value* v) {
          return builder_.CreateICmpNE(
              extract_tag(v), builder_.getInt8(TAG_NIL), "coal.notnil");
        });
  }

  llvm::Value* compile_logical_or(const peg::Ast& ast) {
    return compile_short_circuit(
        ast, "or", [&](llvm::Value* v) { return value_to_bool(v); });
  }

  llvm::Value* compile_logical_and(const peg::Ast& ast) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "and.merge");

    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(),
                                   fn->getEntryBlock().begin());
    auto resultAlloca =
        entryBuilder.CreateAlloca(valueType_, nullptr, "and.tmp");

    for (auto i = 0u; i < ast.nodes.size(); i++) {
      auto val = compile(*ast.nodes[i]);
      builder_.CreateStore(val, resultAlloca);
      auto b = value_to_bool(val);

      if (i < ast.nodes.size() - 1) {
        auto nextBB = llvm::BasicBlock::Create(ctx_, "and.next", fn);
        builder_.CreateCondBr(b, nextBB, mergeBB);
        builder_.SetInsertPoint(nextBB);
      } else {
        builder_.CreateBr(mergeBB);
      }
    }

    fn->insert(fn->end(), mergeBB);
    builder_.SetInsertPoint(mergeBB);
    return builder_.CreateLoad(valueType_, resultAlloca, "and.result");
  }

  // --- Control flow ---

  llvm::Value* compile_if(const peg::Ast& ast) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "if.merge");

    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(),
                                   fn->getEntryBlock().begin());
    auto resultAlloca =
        entryBuilder.CreateAlloca(valueType_, nullptr, "if.tmp");

    // Initialize with nil
    builder_.CreateStore(make_nil(), resultAlloca);

    const auto& nodes = ast.nodes;
    for (auto i = 0u; i < nodes.size(); i += 2) {
      if (i + 1 == nodes.size()) {
        // else block
        auto val = compile(*nodes[i]);
        if (!builder_.GetInsertBlock()->getTerminator()) {
          builder_.CreateStore(val, resultAlloca);
          builder_.CreateBr(mergeBB);
        }
      } else {
        auto cond = compile(*nodes[i]);
        auto b = value_to_bool(cond);

        auto thenBB = llvm::BasicBlock::Create(ctx_, "if.then", fn);
        auto elseBB = llvm::BasicBlock::Create(ctx_, "if.else", fn);
        builder_.CreateCondBr(b, thenBB, elseBB);

        builder_.SetInsertPoint(thenBB);
        auto thenVal = compile(*nodes[i + 1]);
        if (!builder_.GetInsertBlock()->getTerminator()) {
          builder_.CreateStore(thenVal, resultAlloca);
          builder_.CreateBr(mergeBB);
        }

        builder_.SetInsertPoint(elseBB);
      }
    }

    // If no else clause, current block is the last else-BB
    if (nodes.size() % 2 == 0) {
      builder_.CreateBr(mergeBB);
    }

    fn->insert(fn->end(), mergeBB);
    builder_.SetInsertPoint(mergeBB);
    return builder_.CreateLoad(valueType_, resultAlloca, "if.result");
  }

  // Emit IR that tests whether `subject` matches `pattern`.
  // On success, emits variable bindings (via define_var) visible from the
  // current scope; returns i1 true. On failure, returns i1 false; any
  // bindings emitted speculatively are not read because the arm is skipped.
  llvm::Value* emit_pattern(const peg::Ast& pattern, llvm::Value* subject,
                            bool is_mut = false) {
    using namespace peg::udl;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    // OR pattern: PATTERN node with multiple sub-patterns
    if (pattern.tag == "PATTERN"_ && !pattern.nodes.empty()) {
      auto fn = builder_.GetInsertBlock()->getParent();
      auto mergeBB = llvm::BasicBlock::Create(ctx_, "or.match", fn);
      std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;
      for (size_t i = 0; i < pattern.nodes.size(); i++) {
        auto m = emit_pattern(*pattern.nodes[i], subject, is_mut);
        incoming.push_back({m, builder_.GetInsertBlock()});
        if (i + 1 < pattern.nodes.size()) {
          auto nextBB = llvm::BasicBlock::Create(ctx_, "or.try", fn);
          builder_.CreateCondBr(m, mergeBB, nextBB);
          builder_.SetInsertPoint(nextBB);
        } else {
          builder_.CreateBr(mergeBB);
        }
      }
      builder_.SetInsertPoint(mergeBB);
      auto phi = builder_.CreatePHI(builder_.getInt1Ty(),
                                    incoming.size(), "or.result");
      for (auto& [v, bb] : incoming) phi->addIncoming(v, bb);
      return phi;
    }

    switch (pattern.tag) {
      case "WILDCARD"_:
        return builder_.getTrue();
      case "NIL"_:
        return builder_.CreateICmpEQ(extract_tag(subject),
                                     builder_.getInt8(TAG_NIL));
      case "BOOLEAN"_: {
        auto is_bool = builder_.CreateICmpEQ(extract_tag(subject),
                                             builder_.getInt8(TAG_BOOL));
        auto want = builder_.getInt64(pattern.token == "true" ? 1 : 0);
        auto eq = builder_.CreateICmpEQ(extract_data(subject), want);
        return builder_.CreateAnd(is_bool, eq);
      }
      case "NUMBER"_: {
        auto is_long = builder_.CreateICmpEQ(extract_tag(subject),
                                             builder_.getInt8(TAG_LONG));
        auto want = builder_.getInt64(pattern.token_to_number<long>());
        auto eq = builder_.CreateICmpEQ(extract_data(subject), want);
        return builder_.CreateAnd(is_long, eq);
      }
      case "FLOAT"_: {
        auto is_float = builder_.CreateICmpEQ(extract_tag(subject),
                                              builder_.getInt8(TAG_FLOAT));
        auto subjD = builder_.CreateBitCast(
            extract_data(subject), builder_.getDoubleTy(), "pat.d");
        auto want = llvm::ConstantFP::get(
            builder_.getDoubleTy(), pattern.token_to_number<double>());
        // FCmpOEQ: NaN never equals, matching interpreter.
        auto eq = builder_.CreateFCmpOEQ(subjD, want);
        return builder_.CreateAnd(is_float, eq);
      }
      case "STRING"_:
      case "INTERPOLATED_CONTENT"_: {
        // Tag-gate the strcmp via CondBr: an AND would still execute
        // str_eq on the wrong-tag branch and segfault when subject is
        // nil (data=0) or any non-string with a non-pointer data slot.
        auto tag = extract_tag(subject);
        auto is_str = builder_.CreateOr(
            builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_STRING)),
            builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_STRINGVIEW)));
        auto pat_str = pattern.tag == "INTERPOLATED_CONTENT"_
                           ? decode_interpolated_content(pattern.token)
                           : std::string(pattern.token);
        auto lit = builder_.CreateGlobalString(pat_str, ".pat.str");
        auto fn = builder_.GetInsertBlock()->getParent();
        auto eqBB = llvm::BasicBlock::Create(ctx_, "str.eq", fn);
        auto endBB = llvm::BasicBlock::Create(ctx_, "str.end", fn);
        auto entryBB = builder_.GetInsertBlock();
        builder_.CreateCondBr(is_str, eqBB, endBB);

        builder_.SetInsertPoint(eqBB);
        // Materialize StringView → cstr (leak-bounded) so str_eq below
        // can use the existing const char* contract.
        auto subj_ptr = emit_call(
            module_->getFunction(rt::strlike_to_cstr),
            {tag, extract_data(subject)});
        auto eq = emit_call(
            module_->getOrInsertFunction(rt::str_eq,
                                         builder_.getInt1Ty(), ptrTy, ptrTy),
            {subj_ptr, lit});
        auto eqEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, "str.match");
        phi->addIncoming(builder_.getFalse(), entryBB);
        phi->addIncoming(eq, eqEnd);
        return phi;
      }
      case "IDENTIFIER"_: {
        // Always matches; bind subject to this name. `_` is the sink:
        // matches but introduces no binding (subject is borrowed here,
        // so there's nothing to release on this path).
        auto name = std::string(pattern.token);
        if (!is_sink_name(name)) declare_local(name, subject, is_mut);
        return builder_.getTrue();
      }
      case "TYPED_IDENT"_: {
        auto type_name = pattern.nodes[1]->token;
        auto tag = extract_tag(subject);
        // Predicate emitter for a single (non-Union) type name. Pulled
        // into a lambda so Union arms can OR multiple alternatives.
        auto match_single = [&](std::string_view tn) -> llvm::Value* {
          // Strip Generic args before matching: `Array<Long>` matches
          // any Array, mirroring interp's type_matches.
          if (tn.find('<') != std::string_view::npos) {
            tn = culebra::parse_generic_head(tn).outer;
          }
          if (tn == "Any")    return builder_.getTrue();
          if (tn == "Nil")    return builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_NIL));
          if (tn == "Bool")   return builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_BOOL));
          if (tn == "Long")   return builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_LONG));
          if (tn == "Float")  return builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_FLOAT));
          if (tn == "String") return builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_STRING));
          if (tn == "StringView") return builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_STRINGVIEW));
          if (tn == "Array")  return builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_ARRAY));
          if (tn == "Object") return builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));
          if (tn == "Function") return builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_FUNC));
          if (tn == "Tuple")  return builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_TUPLE));
          if (tn == "Set")    return builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_SET));
          if (tn == "Tensor") return builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_TENSOR));
          // Non-primitive name: trait or user class. Delegate to the
          // runtime so trait conformance (StringLike etc.) and class
          // checks share one path with the rest of the type system.
          auto ptrTy = llvm::PointerType::get(ctx_, 0);
          auto nameStr = get_or_create_global_str(std::string(tn), ".typed.tn");
          return emit_call(
              module_->getOrInsertFunction(rt::type_matches,
                                           builder_.getInt1Ty(),
                                           builder_.getInt8Ty(),
                                           builder_.getInt64Ty(), ptrTy),
              {tag, extract_data(subject), nameStr},
              "typed.runtime_match");
        };

        // Union match: OR the predicate over each alternative. Must
        // mirror interp's type_matches recursion to keep both backends
        // in lockstep ([[feedback-check-jit-interp-symmetry]]).
        llvm::Value* tag_match;
        if (type_name.find('|') != std::string_view::npos) {
          tag_match = nullptr;
          for (auto cand : culebra::split_union_types(type_name)) {
            auto m = match_single(cand);
            tag_match = tag_match
                ? builder_.CreateOr(tag_match, m, "union.or")
                : m;
          }
          if (!tag_match) tag_match = builder_.getFalse();
        } else {
          tag_match = match_single(type_name);
        }
        // Bind unconditionally; only used if arm actually runs. `_` is
        // the sink — the type tag still gates the match, but no slot
        // is allocated.
        auto name = std::string(pattern.nodes[0]->token);
        if (!is_sink_name(name)) declare_local(name, subject, is_mut);
        return tag_match;
      }
      case "ARRAY_PATTERN"_:
        return emit_array_pattern(pattern, subject, is_mut);
      case "OBJECT_PATTERN"_:
        return emit_object_pattern(pattern, subject, is_mut);
      case "TUPLE_PATTERN"_:
        return emit_tuple_pattern(pattern, subject, is_mut);
    }
    return builder_.getFalse();
  }

  // Element-by-element destructuring against a JitArray-backed value
  // (Array or Tuple — same storage, different tag). `allow_rest`
  // toggles `...rest` recognition: Array allows it, Tuple's grammar
  // forbids it (the loop simply ignores any REST_PATTERN nodes when
  // false, which is unreachable in practice).
  llvm::Value* emit_indexed_pattern(const peg::Ast& pattern,
                                    llvm::Value* subject,
                                    int8_t expected_tag,
                                    const char* prefix,
                                    bool allow_rest,
                                    bool is_mut = false) {
    using namespace peg::udl;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();
    std::string p(prefix);
    auto failBB = llvm::BasicBlock::Create(ctx_, p + ".fail", fn);
    auto okBB = llvm::BasicBlock::Create(ctx_, p + ".ok", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, p + ".end", fn);

    auto isExpected =
        builder_.CreateICmpEQ(extract_tag(subject),
                              builder_.getInt8(expected_tag));
    auto sizeBB = llvm::BasicBlock::Create(ctx_, p + ".size", fn);
    builder_.CreateCondBr(isExpected, sizeBB, failBB);
    builder_.SetInsertPoint(sizeBB);

    auto arrPtr = builder_.CreateIntToPtr(extract_data(subject), ptrTy);
    auto size = emit_call(
        module_->getOrInsertFunction(rt::array_size,
                                     builder_.getInt64Ty(), ptrTy),
        {arrPtr}, p + ".n");

    const auto& elems = pattern.nodes;
    int rest_idx = -1;
    if (allow_rest) {
      for (size_t i = 0; i < elems.size(); i++) {
        if (elems[i]->tag == "REST_PATTERN"_) {
          rest_idx = static_cast<int>(i);
          break;
        }
      }
    }
    auto fixed = elems.size() - (rest_idx >= 0 ? 1 : 0);

    llvm::Value* size_ok =
        rest_idx < 0
            ? builder_.CreateICmpEQ(size, builder_.getInt64(fixed))
            : builder_.CreateICmpSGE(size, builder_.getInt64(fixed));
    auto elemBB = llvm::BasicBlock::Create(ctx_, p + ".elem", fn);
    builder_.CreateCondBr(size_ok, elemBB, failBB);
    builder_.SetInsertPoint(elemBB);

    auto get_elem = [&](llvm::Value* idx) {
      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      auto outTag = entryB.CreateAlloca(builder_.getInt8Ty(), nullptr,
                                        p + ".tag");
      auto outData = entryB.CreateAlloca(builder_.getInt64Ty(), nullptr,
                                         p + ".data");
      emit_call(
          module_->getOrInsertFunction(
              rt::array_get, builder_.getVoidTy(), ptrTy,
              builder_.getInt64Ty(), ptrTy, ptrTy,
              builder_.getInt64Ty(), builder_.getInt64Ty()),
          {arrPtr, idx, outTag, outData, current_line_val(),
           current_column_val()});
      auto t = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
      auto d = builder_.CreateLoad(builder_.getInt64Ty(), outData);
      llvm::Value* v = llvm::UndefValue::get(valueType_);
      v = builder_.CreateInsertValue(v, t, {0});
      v = builder_.CreateInsertValue(v, d, {1});
      return v;
    };

    auto pre_count =
        rest_idx < 0 ? elems.size() : static_cast<size_t>(rest_idx);
    for (size_t i = 0; i < pre_count; i++) {
      auto v = get_elem(builder_.getInt64(i));
      auto m = emit_pattern(*elems[i], v, is_mut);
      auto contBB = llvm::BasicBlock::Create(ctx_, p + ".cont", fn);
      builder_.CreateCondBr(m, contBB, failBB);
      builder_.SetInsertPoint(contBB);
    }

    if (rest_idx >= 0) {
      auto rest_start = builder_.getInt64(rest_idx);
      auto rest_len =
          builder_.CreateSub(size, builder_.getInt64(fixed), "rest.len");
      auto rest_arr_ptr = emit_call(
          module_->getOrInsertFunction(rt::array_slice, ptrTy,
                                       ptrTy, builder_.getInt64Ty(),
                                       builder_.getInt64Ty()),
          {arrPtr, rest_start, rest_len}, "rest.arr");
      auto rest_val = make_array(rest_arr_ptr);
      auto rest_name = std::string(elems[rest_idx]->nodes[0]->token);
      if (is_sink_name(rest_name)) {
        // `[a, ...] = arr` / `[a, ..._, b] = arr`: still slice the
        // tail (so post-rest elements get the right indices), but
        // drop the resulting Array's +1 instead of holding it.
        emit_value_release(rest_val);
      } else {
        declare_local(rest_name, rest_val, is_mut);
      }
      for (size_t i = rest_idx + 1; i < elems.size(); i++) {
        auto off = static_cast<int64_t>(elems.size() - i);
        auto idx = builder_.CreateSub(size, builder_.getInt64(off));
        auto v = get_elem(idx);
        auto m = emit_pattern(*elems[i], v, is_mut);
        auto contBB = llvm::BasicBlock::Create(ctx_, p + ".cont2", fn);
        builder_.CreateCondBr(m, contBB, failBB);
        builder_.SetInsertPoint(contBB);
      }
    }

    builder_.CreateBr(okBB);

    builder_.SetInsertPoint(okBB);
    auto okEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(failBB);
    auto failEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, p + ".match");
    phi->addIncoming(builder_.getTrue(), okEnd);
    phi->addIncoming(builder_.getFalse(), failEnd);
    return phi;
  }

  llvm::Value* emit_array_pattern(const peg::Ast& pattern,
                                  llvm::Value* subject,
                                  bool is_mut = false) {
    return emit_indexed_pattern(pattern, subject, TAG_ARRAY, "arr",
                                /*allow_rest=*/true, is_mut);
  }

  llvm::Value* emit_tuple_pattern(const peg::Ast& pattern,
                                  llvm::Value* subject,
                                  bool is_mut = false) {
    return emit_indexed_pattern(pattern, subject, TAG_TUPLE, "tup",
                                /*allow_rest=*/false, is_mut);
  }

  llvm::Value* emit_object_pattern(const peg::Ast& pattern,
                                   llvm::Value* subject,
                                   bool is_mut = false) {
    using namespace peg::udl;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();
    auto failBB = llvm::BasicBlock::Create(ctx_, "obj.fail", fn);
    auto okBB = llvm::BasicBlock::Create(ctx_, "obj.ok", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "obj.end", fn);

    auto isObj = builder_.CreateICmpEQ(extract_tag(subject),
                                       builder_.getInt8(TAG_OBJECT));
    auto contBB = llvm::BasicBlock::Create(ctx_, "obj.next", fn);
    builder_.CreateCondBr(isObj, contBB, failBB);
    builder_.SetInsertPoint(contBB);

    auto objPtr = builder_.CreateIntToPtr(extract_data(subject), ptrTy);

    // For each entry: has + get + (bind | recurse into sub-pattern)
    for (const auto& entry : pattern.nodes) {
      std::string key;
      const peg::Ast* sub_pattern = nullptr;
      if (entry->tag == "OBJECT_PAT_ENTRY"_) {
        key = std::string(entry->nodes[0]->token);
        sub_pattern = entry->nodes[1].get();
      } else {
        key = std::string(entry->token);  // shorthand IDENTIFIER
      }
      auto keyPtr = get_or_create_global_str(key, ".obj.key");
      auto has = emit_object_has(objPtr, keyPtr);
      auto next = llvm::BasicBlock::Create(ctx_, "obj.bind", fn);
      builder_.CreateCondBr(has, next, failBB);
      builder_.SetInsertPoint(next);

      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      auto outTag =
          entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "op.tag");
      auto outData =
          entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "op.data");
      emit_call(
          module_->getOrInsertFunction(rt::object_get,
                                       builder_.getVoidTy(), ptrTy, ptrTy,
                                       ptrTy, ptrTy),
          {objPtr, keyPtr, outTag, outData});
      auto t = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
      auto d = builder_.CreateLoad(builder_.getInt64Ty(), outData);
      llvm::Value* v = llvm::UndefValue::get(valueType_);
      v = builder_.CreateInsertValue(v, t, {0});
      v = builder_.CreateInsertValue(v, d, {1});
      if (sub_pattern) {
        // Full `key: PATTERN` form: recursively match the value.
        auto m = emit_pattern(*sub_pattern, v, is_mut);
        auto contBB = llvm::BasicBlock::Create(ctx_, "obj.cont", fn);
        builder_.CreateCondBr(m, contBB, failBB);
        builder_.SetInsertPoint(contBB);
      } else if (is_sink_name(key)) {
        // sink: presence of the key still gates the match (matching
        // the interpreter), but no binding is introduced.
      } else {
        // Retain since we're creating a new owning reference in the slot
        emit_value_retain(v);
        declare_local(key, v, is_mut);
      }
    }

    builder_.CreateBr(okBB);
    builder_.SetInsertPoint(okBB);
    auto okEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(failBB);
    auto failEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, "obj.match");
    phi->addIncoming(builder_.getTrue(), okEnd);
    phi->addIncoming(builder_.getFalse(), failEnd);
    return phi;
  }

  llvm::Value* compile_match(const peg::Ast& ast) {
    using namespace peg::udl;
    auto fn = builder_.GetInsertBlock()->getParent();

    auto subject = compile(*ast.nodes[0]);
    // Stash subject in a local so every arm reads the same SSA value.
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto subjAlloca = entryB.CreateAlloca(valueType_, nullptr, "match.subj");
    builder_.CreateStore(subject, subjAlloca);

    auto endBB = llvm::BasicBlock::Create(ctx_, "match.end", fn);
    auto resultAlloca =
        entryB.CreateAlloca(valueType_, nullptr, "match.result");
    builder_.CreateStore(make_nil(), resultAlloca);

    const auto& arms = ast.nodes[1]->nodes;  // MATCH_ARMS
    for (size_t ai = 0; ai < arms.size(); ai++) {
      auto next_arm_bb =
          llvm::BasicBlock::Create(ctx_, "match.next", fn);
      auto body_bb = llvm::BasicBlock::Create(ctx_, "match.body", fn);

      push_scope();

      auto subj_val = builder_.CreateLoad(valueType_, subjAlloca);
      // Match interp's eval_match → try_pattern (interpreter.h:3728)
      // which uses the default mut=true. Arm-bound names are mutable
      // inside the arm body.
      auto match_cond = emit_pattern(*arms[ai]->nodes[0], subj_val,
                                     /*is_mut=*/true);
      // Either head for guard check or straight to body
      size_t next_idx = 1;
      bool has_guard = arms[ai]->nodes.size() > 1 &&
                       arms[ai]->nodes[1]->tag == "GUARD"_;
      if (has_guard) {
        auto guard_bb =
            llvm::BasicBlock::Create(ctx_, "match.guard", fn);
        builder_.CreateCondBr(match_cond, guard_bb, next_arm_bb);
        builder_.SetInsertPoint(guard_bb);
        auto guard_val = compile(*arms[ai]->nodes[1]->nodes[0]);
        auto guard_bool = value_to_bool(guard_val);
        builder_.CreateCondBr(guard_bool, body_bb, next_arm_bb);
        next_idx = 2;
      } else {
        builder_.CreateCondBr(match_cond, body_bb, next_arm_bb);
      }

      builder_.SetInsertPoint(body_bb);
      auto body_val = compile(*arms[ai]->nodes[next_idx]);
      builder_.CreateStore(body_val, resultAlloca);
      if (!builder_.GetInsertBlock()->getTerminator()) {
        builder_.CreateBr(endBB);
      }

      pop_scope();
      builder_.SetInsertPoint(next_arm_bb);
    }

    // No arm matched → result stays nil
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(endBB);
    return builder_.CreateLoad(valueType_, resultAlloca, "match.res");
  }

  llvm::Value* compile_while(const peg::Ast& ast) {
    auto fn = builder_.GetInsertBlock()->getParent();

    auto condBB = llvm::BasicBlock::Create(ctx_, "while.cond", fn);
    auto bodyBB = llvm::BasicBlock::Create(ctx_, "while.body", fn);
    auto endBB = llvm::BasicBlock::Create(ctx_, "while.end", fn);

    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    auto cond = compile(*ast.nodes[0]);
    auto b = value_to_bool(cond);
    builder_.CreateCondBr(b, bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    loop_stack_.push_back({condBB, endBB});
    compile(*ast.nodes[1]);
    loop_stack_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateBr(condBB);
    }

    builder_.SetInsertPoint(endBB);
    return make_nil();
  }

  // Branch to `target`, then switch the insert point to a fresh dead
  // block so any statements the caller still emits in the same scope
  // land somewhere valid (their IR is simply unreachable).
  void branch_then_dead(llvm::BasicBlock* target, const char* dead_name) {
    builder_.CreateBr(target);
    auto fn = builder_.GetInsertBlock()->getParent();
    auto dead = llvm::BasicBlock::Create(ctx_, dead_name, fn);
    builder_.SetInsertPoint(dead);
  }

  llvm::Value* compile_break(const peg::Ast& ast) {
    if (loop_stack_.empty()) {
      throw culebra::CulebraError("SyntaxError", std::format(
          "break outside loop at {}:{}.", ast.line, ast.column),
          ast.line, ast.column);
    }
    branch_then_dead(loop_stack_.back().break_target, "break.dead");
    return make_nil();
  }

  llvm::Value* compile_continue(const peg::Ast& ast) {
    if (loop_stack_.empty()) {
      throw culebra::CulebraError("SyntaxError", std::format(
          "continue outside loop at {}:{}.", ast.line, ast.column),
          ast.line, ast.column);
    }
    branch_then_dead(loop_stack_.back().continue_target, "continue.dead");
    return make_nil();
  }

  llvm::Value* compile_for(const peg::Ast& ast) {
    auto& id = *ast.nodes[0];
    auto& iter_expr = *ast.nodes[1];
    auto& body = *ast.nodes[2];
    auto var_name = std::string(id.token);

    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();

    auto iterable = compile(iter_expr);
    auto tag = extract_tag(iterable);
    auto data = extract_data(iterable);

    auto arrayBB = llvm::BasicBlock::Create(ctx_, "for.array", fn);
    auto setBB    = llvm::BasicBlock::Create(ctx_, "for.set", fn);
    auto objectBB = llvm::BasicBlock::Create(ctx_, "for.object", fn);
    auto stringBB = llvm::BasicBlock::Create(ctx_, "for.string", fn);
    auto badBB = llvm::BasicBlock::Create(ctx_, "for.bad_type", fn);
    auto endBB = llvm::BasicBlock::Create(ctx_, "for.end", fn);

    auto sw = builder_.CreateSwitch(tag, badBB, 6);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrayBB);
    sw->addCase(builder_.getInt8(TAG_TUPLE), arrayBB);
    sw->addCase(builder_.getInt8(TAG_SET), setBB);
    sw->addCase(builder_.getInt8(TAG_OBJECT), objectBB);
    sw->addCase(builder_.getInt8(TAG_STRING), stringBB);
    sw->addCase(builder_.getInt8(TAG_STRINGVIEW), stringBB);

    builder_.SetInsertPoint(badBB);
    emit_type_error_typed("Array, Tuple, Set, Object, or String", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(arrayBB);
    compile_for_array_loop(builder_.CreateIntToPtr(data, ptrTy),
                           var_name, body, endBB);

    // Set: materialize members into a fresh Array, then reuse the
    // array walker. The temporary Array is +1-owned by this scope;
    // the loop's exits funnel through a cleanup BB that releases it
    // before jumping to endBB. `break` inside the body also lands in
    // the cleanup BB so the release fires regardless of exit path.
    builder_.SetInsertPoint(setBB);
    auto setSrcPtr = builder_.CreateIntToPtr(data, ptrTy);
    auto setMembersArr = emit_call(
        module_->getOrInsertFunction(rt::set_to_array, ptrTy, ptrTy),
        {setSrcPtr}, "for.set.arr");
    auto setCleanupBB = llvm::BasicBlock::Create(ctx_, "for.set.cleanup", fn);
    compile_for_array_loop(setMembersArr, var_name, body, setCleanupBB);
    builder_.SetInsertPoint(setCleanupBB);
    emit_value_release(make_array(setMembersArr));
    builder_.CreateBr(endBB);

    // Object branch splits on whether the receiver carries its own
    // `iter` property:
    //   yes → drive the iterator protocol (Math.range, user-defined
    //         iterators, `.code_points()`/`.graphemes()` et al.)
    //   no  → materialize keys and reuse the native array loop
    // The keys path is unchanged from before, so plain `for k in obj`
    // pays zero overhead vs the prior compiler output.
    builder_.SetInsertPoint(objectBB);
    auto objPtr = builder_.CreateIntToPtr(data, ptrTy);
    auto iterKeyPtr = get_or_create_global_str("iter", ".iter.key");
    auto hasIter = emit_object_has(objPtr, iterKeyPtr);
    auto keysBB = llvm::BasicBlock::Create(ctx_, "for.obj.keys", fn);
    auto protoBB = llvm::BasicBlock::Create(ctx_, "for.obj.proto", fn);
    builder_.CreateCondBr(hasIter, protoBB, keysBB);

    builder_.SetInsertPoint(keysBB);
    // No user-defined `iter`: drive the builtin object_iter so the same
    // mut_count fail-fast that protects `obj.iter()` also covers the
    // `for k in obj` sugar. Reuses the protocol loop below.
    auto objIter = emit_call(module_->getFunction(rt::object_iter),
                             {objPtr});
    llvm::Value* keysIterVal = llvm::UndefValue::get(valueType_);
    keysIterVal = builder_.CreateInsertValue(
        keysIterVal, builder_.getInt8(TAG_OBJECT), {0});
    keysIterVal = builder_.CreateInsertValue(
        keysIterVal, builder_.CreatePtrToInt(objIter, builder_.getInt64Ty()),
        {1});
    compile_for_protocol_loop(keysIterVal, var_name, body, endBB);

    builder_.SetInsertPoint(protoBB);
    llvm::Value* objVal = llvm::UndefValue::get(valueType_);
    objVal = builder_.CreateInsertValue(objVal, builder_.getInt8(TAG_OBJECT),
                                        {0});
    objVal = builder_.CreateInsertValue(objVal, data, {1});
    compile_for_protocol_loop(objVal, var_name, body, endBB);

    builder_.SetInsertPoint(stringBB);
    // For StringView, materialize via strlike_to_cstr (TAG_STRING input
    // returns the same ptr — no copy).
    auto strDataPtr = emit_call(
        module_->getFunction(rt::strlike_to_cstr), {tag, data});
    compile_for_string_loop(strDataPtr, var_name, body, endBB);

    builder_.SetInsertPoint(endBB);
    return make_nil();
  }

  // Bind `elemVal` to `var_name` for the body of a for-in. Caller must
  // have transferred `+1` into `elemVal`; the slot takes over that ref
  // and releases on scope exit. Useful when the caller has already
  // arranged ownership (e.g. the protocol loop pre-retains the step
  // value before releasing the enclosing {done,value} object).
  void emit_for_body_with_owned_binding(const std::string& var_name,
                                        llvm::Value* elemVal,
                                        const peg::Ast& body,
                                        llvm::BasicBlock* continue_bb,
                                        llvm::BasicBlock* break_bb) {
    push_scope();
    if (is_sink_name(var_name)) {
      // `for _ in iter`: drop the per-iteration +1 transfer that the
      // caller already emitted (no slot will release it for us).
      emit_value_release(elemVal);
    } else {
      declare_local(var_name, elemVal);
    }

    loop_stack_.push_back({continue_bb, break_bb});
    compile(body);
    loop_stack_.pop_back();

    pop_scope();

    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateBr(continue_bb);
    }
  }

  // Borrowed-input variant: retains `elemVal` once (to balance the
  // slot release) and delegates. Used by the Array / String for-in
  // loops where `elemVal` is read directly out of the container with
  // no owning ref of its own.
  void emit_for_body_iteration(const std::string& var_name,
                               llvm::Value* elemVal,
                               const peg::Ast& body,
                               llvm::BasicBlock* continue_bb,
                               llvm::BasicBlock* break_bb) {
    emit_value_retain(elemVal);
    emit_for_body_with_owned_binding(var_name, elemVal, body, continue_bb,
                                     break_bb);
  }

  void compile_for_array_loop(llvm::Value* arrPtr,
                              const std::string& var_name,
                              const peg::Ast& body,
                              llvm::BasicBlock* endBB) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();

    auto size = emit_call(
        module_->getOrInsertFunction(rt::array_size, builder_.getInt64Ty(),
                                     ptrTy),
        {arrPtr}, "for.size");

    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto idxAlloca =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "for.idx");
    auto outTag =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "for.out.tag");
    auto outData =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "for.out.data");
    builder_.CreateStore(builder_.getInt64(0), idxAlloca);

    auto condBB = llvm::BasicBlock::Create(ctx_, "for.a.cond", fn);
    auto bodyBB = llvm::BasicBlock::Create(ctx_, "for.a.body", fn);
    auto incBB = llvm::BasicBlock::Create(ctx_, "for.a.inc", fn);

    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    auto idx = builder_.CreateLoad(builder_.getInt64Ty(), idxAlloca, "for.i");
    builder_.CreateCondBr(builder_.CreateICmpSGE(idx, size), endBB, bodyBB);

    builder_.SetInsertPoint(bodyBB);
    emit_call(
        module_->getOrInsertFunction(
            rt::array_get, builder_.getVoidTy(), ptrTy,
            builder_.getInt64Ty(), ptrTy, ptrTy, builder_.getInt64Ty(),
            builder_.getInt64Ty()),
        {arrPtr, idx, outTag, outData, current_line_val(),
         current_column_val()});
    auto t = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
    auto d = builder_.CreateLoad(builder_.getInt64Ty(), outData);
    llvm::Value* elem = llvm::UndefValue::get(valueType_);
    elem = builder_.CreateInsertValue(elem, t, {0});
    elem = builder_.CreateInsertValue(elem, d, {1});

    emit_for_body_iteration(var_name, elem, body, incBB, endBB);

    builder_.SetInsertPoint(incBB);
    auto next = builder_.CreateAdd(
        builder_.CreateLoad(builder_.getInt64Ty(), idxAlloca),
        builder_.getInt64(1));
    builder_.CreateStore(next, idxAlloca);
    builder_.CreateBr(condBB);
  }

  // Drive the iterator protocol for objects that carry a user-defined
  // `iter` property (Math.range, `.code_points()`, custom iterators).
  // Semantics match `_get_iterator` / `eval_for`'s protocol branch in
  // the interpreter: `obj.iter()` yields an iterator Object; then each
  // `next()` returns `{done, value}` until `done == true`.
  //
  // Refcount flow: `iter()` returns a fresh `+1` iterator value stored
  // in a local slot so it survives across iterations. Each method call
  // retains its receiver before invocation (the callee frame releases
  // on exit via the owned `this` slot). `step` — the {done, value}
  // object — is released on every loop path before branching.
  void compile_for_protocol_loop(llvm::Value* objVal,
                                 const std::string& var_name,
                                 const peg::Ast& body,
                                 llvm::BasicBlock* endBB) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    // iter_val = obj.iter()
    auto iter_fn_val = compile_property_get(objVal, "iter");
    emit_value_retain(objVal);  // handed off to `this` slot
    auto iter_val = compile_function_call_raw(iter_fn_val, objVal, {});

    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto iterAlloca = entryB.CreateAlloca(valueType_, nullptr, "for.iter");
    entryB.CreateStore(llvm::ConstantAggregateZero::get(valueType_),
                       iterAlloca);
    builder_.CreateStore(iter_val, iterAlloca);

    // `has_next` + `next` are fixed for an iterator's lifetime — hoist
    // both lookups so each step is a pair of cached closure dispatches
    // rather than a per-iteration `find_slot`.
    auto has_next_fn_val = compile_property_get(iter_val, "has_next");
    auto has_next_cls_ptr =
        builder_.CreateIntToPtr(extract_data(has_next_fn_val), ptrTy,
                                "has_next.cls");
    auto next_fn_val = compile_property_get(iter_val, "next");
    auto next_cls_ptr =
        builder_.CreateIntToPtr(extract_data(next_fn_val), ptrTy, "next.cls");

    auto outTagAlloca =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "for.p.out_tag");
    auto outDataAlloca =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "for.p.out_data");

    auto condBB = llvm::BasicBlock::Create(ctx_, "for.p.cond", fn);
    auto bodyBB = llvm::BasicBlock::Create(ctx_, "for.p.body", fn);
    auto cleanupBB = llvm::BasicBlock::Create(ctx_, "for.p.cleanup", fn);

    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    auto iterCur = builder_.CreateLoad(valueType_, iterAlloca, "iter.cur");
    auto iterTag = extract_tag(iterCur);
    auto iterData = extract_data(iterCur);
    auto ok = emit_call(
        module_->getFunction(rt::iter_advance),
        {has_next_cls_ptr, next_cls_ptr, iterTag, iterData,
         outTagAlloca, outDataAlloca},
        "for.p.ok");
    auto alive = builder_.CreateICmpNE(ok, builder_.getInt64(0),
                                       "for.p.alive");
    builder_.CreateCondBr(alive, bodyBB, cleanupBB);

    builder_.SetInsertPoint(bodyBB);
    auto outTag =
        builder_.CreateLoad(builder_.getInt8Ty(), outTagAlloca, "for.p.tag");
    auto outData = builder_.CreateLoad(builder_.getInt64Ty(), outDataAlloca,
                                       "for.p.data");
    llvm::Value* loop_val = llvm::UndefValue::get(valueType_);
    loop_val = builder_.CreateInsertValue(loop_val, outTag, {0});
    loop_val = builder_.CreateInsertValue(loop_val, outData, {1});
    emit_for_body_with_owned_binding(var_name, loop_val, body, condBB,
                                     cleanupBB);

    // cleanupBB: release the iterator slot contents, exit the for-in
    builder_.SetInsertPoint(cleanupBB);
    auto iterFinal = builder_.CreateLoad(valueType_, iterAlloca, "iter.fin");
    emit_value_release(iterFinal);
    builder_.CreateBr(endBB);
  }

  void compile_for_string_loop(llvm::Value* strPtr,
                               const std::string& var_name,
                               const peg::Ast& body,
                               llvm::BasicBlock* endBB) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();

    auto len = emit_call(
        module_->getOrInsertFunction(rt::str_size, builder_.getInt64Ty(),
                                     ptrTy),
        {strPtr}, "for.slen");

    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto offAlloca =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "for.off");
    builder_.CreateStore(builder_.getInt64(0), offAlloca);

    auto condBB = llvm::BasicBlock::Create(ctx_, "for.s.cond", fn);
    auto bodyBB = llvm::BasicBlock::Create(ctx_, "for.s.body", fn);
    auto incBB = llvm::BasicBlock::Create(ctx_, "for.s.inc", fn);

    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    auto off = builder_.CreateLoad(builder_.getInt64Ty(), offAlloca, "for.o");
    auto scalarLen = emit_call(
        module_->getOrInsertFunction(rt::utf8_scalar_len,
                                     builder_.getInt64Ty(), ptrTy,
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {strPtr, off, len}, "for.slen1");
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(scalarLen, builder_.getInt64(0)), endBB,
        bodyBB);

    builder_.SetInsertPoint(bodyBB);
    auto scalarPtr = emit_call(
        module_->getOrInsertFunction(rt::str_scalar_at, ptrTy, ptrTy,
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {strPtr, off, scalarLen}, "for.scalar");
    emit_for_body_iteration(var_name, make_string(scalarPtr), body, incBB,
                            endBB);

    builder_.SetInsertPoint(incBB);
    auto next = builder_.CreateAdd(
        builder_.CreateLoad(builder_.getInt64Ty(), offAlloca), scalarLen);
    builder_.CreateStore(next, offAlloca);
    builder_.CreateBr(condBB);
  }

  // --- Lexical scope ---

  llvm::Value* compile_lexical_scope(const peg::Ast& ast) {
    // No defers in this scope → only variable-binding scoping needed.
    bool has_defer = scope_has_defer_.contains(&ast);

    llvm::Value* mark = nullptr;
    llvm::BasicBlock* cleanupBB = nullptr;
    llvm::BasicBlock* savedLpad = current_lpad_;
    if (has_defer) {
      // defer_mark is nothrow, so a plain call (not invoke) is safe.
      mark = builder_.CreateCall(
          module_->getFunction(rt::defer_mark), {}, "scope.mark");
      auto fn = builder_.GetInsertBlock()->getParent();
      cleanupBB = llvm::BasicBlock::Create(ctx_, "scope.cleanup", fn);
      current_lpad_ = cleanupBB;
    }

    push_scope();
    llvm::Value* val = make_nil();
    for (auto& node : ast.nodes) {
      if (builder_.GetInsertBlock()->getTerminator()) break;
      val = compile(*node);
    }
    // Statement-like: drop the block's final expression before
    // pop_scope so `drop` callbacks fire in true LIFO order (a retained
    // expression result would otherwise outlive the slot release).
    // Matches the interpreter's standalone-block semantics.
    if (!builder_.GetInsertBlock()->getTerminator()) {
      emit_value_release(val);
    }
    val = make_nil();
    pop_scope();
    current_lpad_ = savedLpad;

    if (has_defer && !builder_.GetInsertBlock()->getTerminator()) {
      emit_call(module_->getFunction(rt::defer_run_to),
                {mark});
    }
    auto afterBB = builder_.GetInsertBlock();

    if (has_defer) {
      emit_cleanup_landingpad(cleanupBB, mark, savedLpad, "scope.exc");
    }

    builder_.SetInsertPoint(afterBB);
    return val;
  }

  // --- Function ---

  // Build an LLVM function that serves as the `new` dispatcher for a
  // class declaration. At runtime this function:
  //
  //   1. Reads its captured cells to recover (a) the shared class
  //      meta object (built once per class declaration; holds the
  //      method closures via proto delegation) and (b) the user's
  //      `new` body closure if present.
  //   2. Calls `rt::build_class_instance` to allocate the instance,
  //      wire its proto at the meta, run the user body if any, and
  //      promote the instance's own data slots to mutable.
  //
  // The caller's `this` (the class namespace, carried with +1 per ABI)
  // is released up front since the constructor has no use for it.
  llvm::Function* emit_constructor_fn(std::string_view class_name,
                                       bool has_new, size_t arity) {
    using namespace llvm;
    auto ptrTy = PointerType::get(ctx_, 0);
    auto i64Ty = builder_.getInt64Ty();
    auto i8Ty = builder_.getInt8Ty();

    auto fnType = FunctionType::get(
        valueType_, {ptrTy, valueType_, i64Ty, ptrTy}, false);
    auto fnName = std::format("__culebra_ctor_{}", funcCounter_++);
    auto fn = Function::Create(fnType, GlobalValue::ExternalLinkage, fnName,
                               module_);

    auto savedIP = builder_.saveIP();
    auto argIt = fn->arg_begin();
    auto clsArg = &*argIt++;
    auto thisArg = &*argIt++;
    auto nArgsArg = &*argIt++;
    auto argsArg = &*argIt++;
    clsArg->setName("__cls__");
    thisArg->setName("this");
    nArgsArg->setName("n_args");
    argsArg->setName("args");

    auto entryBB = BasicBlock::Create(ctx_, "entry", fn);
    builder_.SetInsertPoint(entryBB);

    // Caller's `this` (the class namespace) is unused — release the +1.
    emit_value_release(thisArg);

    auto capturesFieldPtr =
        builder_.CreateStructGEP(closureType_, clsArg, 3);
    auto capturesArr = builder_.CreateLoad(ptrTy, capturesFieldPtr);

    auto load_capture = [&](size_t i) {
      auto cellSlotPtr = builder_.CreateInBoundsGEP(
          ptrTy, capturesArr,
          {builder_.getInt64(static_cast<int64_t>(i))});
      auto cellPtr = builder_.CreateLoad(ptrTy, cellSlotPtr);
      auto valuePtr = builder_.CreateStructGEP(cellType_, cellPtr, 1);
      return builder_.CreateLoad(valueType_, valuePtr);
    };

    // Capture[0] is the shared class meta (TAG_OBJECT). Capture[1] is
    // the user `new` body (TAG_FUNC) when present.
    auto metaVal = load_capture(0);
    auto metaPtr = builder_.CreateIntToPtr(
        builder_.CreateExtractValue(metaVal, {1}), ptrTy, "meta.ptr");

    llvm::Value* bodyTag;
    llvm::Value* bodyData;
    if (has_new) {
      auto bodyVal = load_capture(1);
      bodyTag = builder_.CreateExtractValue(bodyVal, {0});
      bodyData = builder_.CreateExtractValue(bodyVal, {1});
    } else {
      bodyTag = builder_.getInt8(TAG_NIL);
      bodyData = builder_.getInt64(0);
    }

    auto classNameGlobal =
        get_or_create_global_str(class_name, ".class.name");

    auto result = builder_.CreateCall(
        module_->getOrInsertFunction(rt::build_class_instance, valueType_,
                                     ptrTy, ptrTy, i8Ty, i64Ty, i64Ty,
                                     ptrTy),
        {classNameGlobal, metaPtr, bodyTag, bodyData, nArgsArg, argsArg});
    builder_.CreateRet(result);

    verifyFunction(*fn);
    builder_.restoreIP(savedIP);
    (void)arity;
    return fn;
  }

  // `trait Name { req(); def() { ... } }` — register a structural
  // trait contract in the shared `culebra::trait_registry()` so
  // type_matches / dispatch can see it. Default method bodies are
  // compiled into JitClosures and stashed in `_jit_trait_default_impls`
  // for the property-access fallback path.
  llvm::Value* compile_trait_decl(const peg::Ast& ast) {
    using namespace peg::udl;
    size_t k = 0;
    while (k < ast.nodes.size() && ast.nodes[k]->tag == "DECORATOR"_) k++;
    auto head = culebra::parse_generic_head(ast.nodes[k]->token);
    std::string trait_name(head.outer);

    culebra::TraitDef def;
    def.name = trait_name;
    auto& defaults = _jit_trait_default_impls()[trait_name];
    defaults.clear();

    for (size_t i = k + 1; i < ast.nodes.size(); i++) {
      const auto& m = *ast.nodes[i];
      auto tv = culebra::view_trait_method(m);
      // Arity count (positional only).
      size_t arity = 0;
      for (const auto& p : tv.params->nodes) {
        if (culebra::is_kw_only_sep(*p) || culebra::is_kwargs_rest(*p))
          continue;
        arity++;
      }
      bool has_body = static_cast<bool>(tv.body);
      def.methods.push_back({std::string(tv.name), arity, has_body});

      // Emit runtime registration of this method so the trait is
      // populated when the AOT binary runs (and harmless on JIT).
      {
        auto ptrTy = llvm::PointerType::get(ctx_, 0);
        auto trait_g = builder_.CreateGlobalString(
            trait_name,
            ".trait." + trait_name + "." + std::string(tv.name) + ".tn");
        auto method_g = builder_.CreateGlobalString(
            std::string(tv.name),
            ".trait." + trait_name + "." + std::string(tv.name) + ".mn");
        emit_call(
            module_->getOrInsertFunction(
                rt::register_trait_method,
                builder_.getVoidTy(), ptrTy, ptrTy,
                builder_.getInt64Ty(), builder_.getInt8Ty()),
            {trait_g, method_g,
             builder_.getInt64(static_cast<int64_t>(arity)),
             builder_.getInt8(has_body ? 1 : 0)});
      }

      if (tv.body) {
        // Compile the default-method body as a closure. analyze_fn_body
        // has already populated func_info_[&m] via visit_for_frees.
        auto* fn_val_ir = compile_fn_common(
            &m, *tv.params, tv.body, /*returnType=*/{},
            std::string(tv.name));
        // fn_val_ir is a JitValue (struct {i8 tag, i64 data}); the data
        // is the JitClosure*. Pull it out and register the default into
        // the runtime trait-default table.
        auto ptrTy = llvm::PointerType::get(ctx_, 0);
        auto closure_data =
            builder_.CreateExtractValue(fn_val_ir, /*idx=*/1, "td.data");
        auto closure_ptr =
            builder_.CreateIntToPtr(closure_data, ptrTy, "td.cls");
        auto trait_g = builder_.CreateGlobalString(
            trait_name, ".trait." + trait_name);
        auto method_g = builder_.CreateGlobalString(
            std::string(tv.name),
            ".td." + trait_name + "." + std::string(tv.name));
        emit_call(
            module_->getOrInsertFunction(
                rt::register_trait_default,
                builder_.getVoidTy(), ptrTy, ptrTy, ptrTy),
            {trait_g, method_g, closure_ptr});
      }
    }
    culebra::register_trait(std::move(def));
    return llvm::UndefValue::get(valueType_);
  }

  // `class Name { new(...){...}  m(...){...} }` — compiles each method
  // plus the user-new body as regular closures, then emits a synthetic
  // constructor closure that delegates to the runtime instance builder.
  // `Name` binds the resulting namespace object in the current scope.
  llvm::Value* compile_class_decl(const peg::Ast& ast) {
    using namespace peg::udl;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    // AST: [DECORATOR*, IDENTIFIER, METHOD ...]
    size_t dec_end = 0;
    while (dec_end < ast.nodes.size() &&
           ast.nodes[dec_end]->tag == "DECORATOR"_) {
      dec_end++;
    }

    // CLASS_HEAD may carry Generic type params (`Box<T>`); strip them
    // — the runtime sees only the outer name, type params are docs.
    auto class_head = culebra::parse_generic_head(
        ast.nodes[dec_end]->token);
    std::string class_name(class_head.outer);
    // Construct the guard BEFORE the populate step so any exception
    // (allocation failure in split_generic_args, etc.) still restores
    // the outer class's params on unwind.
    struct ClassScopeGuard {
      std::vector<std::string_view>& slot;
      std::vector<std::string_view> saved;
      ~ClassScopeGuard() { slot = std::move(saved); }
    } class_scope_guard{class_type_params_,
                        std::move(class_type_params_)};
    class_type_params_.clear();
    if (!class_head.args.empty()) {
      class_type_params_ = culebra::split_generic_args(class_head.args);
    }

    for (size_t i = dec_end + 1; i < ast.nodes.size(); i++) {
      auto mv = culebra::view_method(*ast.nodes[i]);
      culebra::reject_class_decl_in_class_body(
          mv.is_field ? *mv.value : **mv.body, class_name);
    }

    const peg::Ast* new_ast = nullptr;
    std::vector<std::string> method_names;
    std::vector<const peg::Ast*> method_asts;
    std::vector<std::string> static_names;
    std::vector<const peg::Ast*> static_asts;
    std::vector<std::string> static_field_names;
    std::vector<const peg::Ast*> static_field_asts;
    for (size_t i = dec_end + 1; i < ast.nodes.size(); i++) {
      const auto& m = *ast.nodes[i];
      auto mv = culebra::view_method(m);
      if (mv.is_field) {
        culebra::require_static_field(mv, class_name);
        static_field_names.push_back(std::string(mv.name));
        static_field_asts.push_back(&m);
        continue;
      }
      auto name = std::string(mv.name);
      if (!mv.is_static && mv.name == "new") {
        new_ast = &m;
      } else if (mv.is_static) {
        static_names.push_back(std::move(name));
        static_asts.push_back(&m);
      } else {
        method_names.push_back(std::move(name));
        method_asts.push_back(&m);
      }
    }

    // Pre-allocate a cell for `Name` so method closures — which will
    // almost always reference the class for `Name.new(...)` or match
    // tagging — can capture it before the namespace itself exists.
    // The cell starts nil; we patch it to the real class value once
    // the methods have been compiled and the ctor closure is built.
    // Reuse a forward-ref pre-allocated slot if one already exists in
    // the current scope (statements above this class may have captured
    // it). Otherwise create a fresh cell.
    VarSlot classSlot;
    if (!scopes_.empty()) {
      auto& curSlots = scopes_.back().slots;
      auto it = curSlots.find(class_name);
      if (it != curSlots.end() && it->second.kind == VarSlot::Cell) {
        classSlot = it->second;
      } else {
        auto nilVal = make_nil();
        classSlot = make_cell_slot(class_name, nilVal);
        define_var(class_name, classSlot);
      }
    } else {
      auto nilVal = make_nil();
      classSlot = make_cell_slot(class_name, nilVal);
      define_var(class_name, classSlot);
    }

    // Compile each method into a closure %Value (+1 owned).
    std::vector<llvm::Value*> method_vals;
    method_vals.reserve(method_asts.size());
    for (auto* m : method_asts) {
      method_vals.push_back(compile_function(*m));
    }
    llvm::Value* body_val = nullptr;
    size_t new_arity = 0;
    if (new_ast) {
      body_val = compile_function(*new_ast);
      new_arity = culebra::view_method(*new_ast).params->nodes.size();
    }

    std::vector<llvm::Value*> static_vals;
    static_vals.reserve(static_asts.size());
    for (auto* m : static_asts) {
      static_vals.push_back(compile_function(*m));
    }

    std::vector<llvm::Value*> static_field_vals;
    static_field_vals.reserve(static_field_asts.size());
    for (auto* m : static_field_asts) {
      static_field_vals.push_back(compile(*m->nodes[2]));
    }

    // Build the shared class meta object once per class declaration:
    // a JitObject with all method closures set as immutable props.
    // Each instance points its `proto` at this meta, so special-method
    // dispatch + general property lookup fall through here without
    // copying the methods into every instance's own props.
    llvm::Value* metaPtr;
    {
      llvm::Value* methodSlab;
      llvm::Value* nameArrayPtr;
      size_t n_methods = method_vals.size();
      if (n_methods > 0) {
        methodSlab = builder_.CreateAlloca(
            valueType_, builder_.getInt64(static_cast<int64_t>(n_methods)),
            "meta.methods");
        for (size_t i = 0; i < n_methods; i++) {
          auto slot = builder_.CreateInBoundsGEP(
              valueType_, methodSlab,
              {builder_.getInt64(static_cast<int64_t>(i))});
          builder_.CreateStore(method_vals[i], slot);
        }
        std::vector<llvm::Constant*> names;
        names.reserve(n_methods);
        for (const auto& m : method_names) {
          names.push_back(llvm::cast<llvm::Constant>(
              get_or_create_global_str(m, ".mname")));
        }
        auto nameArrayTy = llvm::ArrayType::get(ptrTy, n_methods);
        auto nameArrayConst = llvm::ConstantArray::get(nameArrayTy, names);
        nameArrayPtr = new llvm::GlobalVariable(
            *module_, nameArrayTy, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, nameArrayConst, ".mnames");
      } else {
        methodSlab = llvm::ConstantPointerNull::get(ptrTy);
        nameArrayPtr = llvm::ConstantPointerNull::get(ptrTy);
      }
      metaPtr = emit_call(
          module_->getOrInsertFunction(rt::build_class_meta, ptrTy, ptrTy,
                                       ptrTy, builder_.getInt64Ty()),
          {nameArrayPtr, methodSlab,
           builder_.getInt64(static_cast<int64_t>(n_methods))},
          "class.meta");
    }
    auto metaVal = make_object(metaPtr);

    auto ctor_fn =
        emit_constructor_fn(class_name, new_ast != nullptr, new_arity);

    // Captures: meta first, then body if present. (Methods themselves
    // were transferred into the meta object above and don't need to
    // live separately in the closure.)
    size_t n_captures = 1 + (new_ast ? 1 : 0);
    auto closurePtr = emit_call(
        module_->getOrInsertFunction(rt::closure_new, ptrTy, ptrTy,
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {ctor_fn,
         builder_.getInt64(static_cast<int64_t>(n_captures)),
         builder_.getInt64(static_cast<int64_t>(new_arity))},
        "ctor.closure");

    auto capturesFieldPtr =
        builder_.CreateStructGEP(closureType_, closurePtr, 3);
    auto capturesArr = builder_.CreateLoad(ptrTy, capturesFieldPtr);
    auto install_capture = [&](size_t i, llvm::Value* v) {
      // cell_new takes ownership of the %Value's +1 by storing the raw
      // tag/data without retaining. Storing the cell pointer into the
      // captures array likewise transfers the cell's +1 to the closure.
      auto cellPtr = emit_call(
          module_->getOrInsertFunction(rt::cell_new, ptrTy,
                                       builder_.getInt8Ty(),
                                       builder_.getInt64Ty()),
          {extract_tag(v), extract_data(v)}, "ctor.cell");
      auto dst = builder_.CreateInBoundsGEP(
          ptrTy, capturesArr,
          {builder_.getInt64(static_cast<int64_t>(i))});
      builder_.CreateStore(cellPtr, dst);
    };
    install_capture(0, metaVal);
    if (body_val) {
      install_capture(1, body_val);
    }

    // Wrap the constructor closure into a Value(TAG_FUNC) for storage.
    auto ctorVal = make_func(closurePtr);

    // Build the class namespace object: { new: ctorClosure }.
    auto classObj = emit_call(
        module_->getOrInsertFunction(rt::object_new, ptrTy), {},
        "class.ns");
    emit_object_set(classObj, "new", /*mut=*/false, extract_tag(ctorVal),
                    extract_data(ctorVal));
    for (size_t i = 0; i < static_vals.size(); i++) {
      emit_object_set(classObj, static_names[i], /*mut=*/false,
                      extract_tag(static_vals[i]),
                      extract_data(static_vals[i]));
    }
    for (size_t i = 0; i < static_field_vals.size(); i++) {
      emit_object_set(classObj, static_field_names[i], /*mut=*/false,
                      extract_tag(static_field_vals[i]),
                      extract_data(static_field_vals[i]));
    }
    auto classVal = make_object(classObj);

    // Apply decorators (bottom-up): each takes the current class
    // value and returns the new one. Decorated class still ends up
    // in the same `classSlot` so method-capture references resolve
    // to the decorated value at call time.
    for (size_t i = dec_end; i > 0; --i) {
      const auto& dec_expr = *ast.nodes[i - 1]->nodes[0];
      auto decoCallee = compile(dec_expr);
      classVal = compile_function_call_raw(decoCallee, nullptr, {classVal});
    }

    // Patch the pre-allocated cell with the real class namespace.
    // `store_slot` transfers `classVal`'s +1 into the cell. Any
    // further use of `classVal` (REPL persist below) must retain
    // its own reference; nothing else here consumes the SSA value.
    store_slot(classSlot, classVal);

    // REPL: also persist the class namespace into session globals so
    // subsequent inputs can reference `Name.new(...)` etc. The local
    // cell stays live because compiled methods captured it; the
    // globals entry is the channel later inputs read through. +1
    // source: `classVal`'s +1 was already consumed by the
    // `store_slot` above (writing into the method-capture cell), so
    // we retain to give `repl_set` its own ownership.
    if (is_repl_top_level_ && is_repl_session_) {
      emit_value_retain(classVal);
      emit_repl_persist(classVal, class_name, ".repl.class",
                        /*is_let=*/true, /*is_mut=*/false);
    }
    return make_nil();
  }

  // IMPORT_STMT: [IDENTIFIER, STRING]. The dependency was loaded and
  // registered (in `_jit_module_table`) by run_modules before this
  // module's IR was emitted; the helper just pulls the export Object
  // back out, retains it for the importing scope, and the local cell
  // takes ownership.
  llvm::Value* compile_import_stmt(const peg::Ast& ast) {
    auto name = std::string(ast.nodes[0]->token);
    auto rel = std::string(ast.nodes[1]->token);
    if (current_module_path_.empty()) {
      throw culebra::CulebraError(
          "ImportError",
          "`import` is not supported in this context (REPL or direct "
          "eval); run via `culebra script.cul`",
          ast.line, ast.column);
    }
    auto canon = culebra::resolve_module_path(
        rel, current_module_path_.parent_path());

    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto tagSlot =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "mod.tag");
    auto dataSlot =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "mod.data");
    auto pathPtr = get_or_create_global_str(canon.string(), ".modpath");
    emit_call(
        module_->getOrInsertFunction(
            rt::module_get, builder_.getVoidTy(),
            ptrTy, ptrTy, ptrTy,
            builder_.getInt64Ty(), builder_.getInt64Ty()),
        {pathPtr, tagSlot, dataSlot,
         builder_.getInt64(ast.line), builder_.getInt64(ast.column)});

    auto tag = builder_.CreateLoad(builder_.getInt8Ty(), tagSlot,
                                    "mod.tag.v");
    auto data = builder_.CreateLoad(builder_.getInt64Ty(), dataSlot,
                                     "mod.data.v");
    llvm::Value* v = llvm::UndefValue::get(valueType_);
    v = builder_.CreateInsertValue(v, tag, {0});
    v = builder_.CreateInsertValue(v, data, {1});
    declare_local(name, v);
    return make_nil();
  }

  // After a dependency module's body has been compiled, walk its AST
  // for EXPORT_STMT entries, build a single Object holding every named
  // binding, and hand it to the runtime module table. Subsequent
  // IMPORT_STMT compilations (in dependents) load the table back out.
  void emit_build_and_register_export(const peg::Ast& module_ast,
                                       const std::string& abs_path) {
    using namespace peg::udl;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto objPtr = emit_call(
        module_->getOrInsertFunction(rt::object_new, ptrTy), {},
        "mod.export");

    const peg::Ast* stmts =
        module_ast.tag == "STATEMENTS"_ ||
                module_ast.original_tag == "STATEMENTS"_
            ? &module_ast
            : (module_ast.nodes.empty() ? nullptr
                                         : module_ast.nodes[0].get());
    if (stmts) {
      for (const auto& s : stmts->nodes) {
        if (s->tag != "EXPORT_STMT"_) continue;
        for (const auto& id : s->nodes) {
          auto name = std::string(id->token);
          // compile_identifier walks locals → builtin namespace → REPL
          // globals and raises NameError at runtime on a miss, matching
          // interp's env->get / NameError path in extract_export.
          auto val = compile_identifier(*id);
          emit_object_set(objPtr, name, /*mut=*/false,
                          extract_tag(val), extract_data(val));
        }
      }
    }

    auto objVal = make_object(objPtr);
    auto pathPtr = get_or_create_global_str(abs_path, ".modpath");
    emit_call(
        module_->getOrInsertFunction(rt::module_register,
                                      builder_.getVoidTy(),
                                      ptrTy, builder_.getInt8Ty(),
                                      builder_.getInt64Ty()),
        {pathPtr, extract_tag(objVal), extract_data(objVal)});
  }

  // `fn name(params) body` — top-level multimethod declaration. Compiles
  // the body as a normal closure and hands ownership to the runtime
  // multimethod table; the runtime returns a dispatcher closure (created
  // on the first decl per name and cached afterward) which we bind to
  // `name` in the surrounding scope. See _jit_multifn_dispatcher_thunk
  // for the dispatch logic. JIT counterpart of eval_multifn_decl.
  llvm::Value* compile_multifn_decl(const peg::Ast& ast) {
    using namespace peg::udl;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i64Ty = builder_.getInt64Ty();

    // AST: [DECORATOR*, CLASS_HEAD, PARAMETERS, [RETURN_TYPE,] BLOCK]
    size_t dec_end = 0;
    while (dec_end < ast.nodes.size() &&
           ast.nodes[dec_end]->tag == "DECORATOR"_) {
      dec_end++;
    }
    bool has_decorators = dec_end > 0;

    // CLASS_HEAD may carry Generic params (`sort<T: Comparable>`); the
    // bound name is the outer only. Mirrors compile_class_decl.
    std::string name(culebra::parse_generic_head(
        ast.nodes[dec_end]->token).outer);
    const peg::Ast* params_ast = ast.nodes[dec_end + 1].get();
    size_t body_idx = dec_end + 2;
    std::string_view returnType;
    if (body_idx < ast.nodes.size() &&
        ast.nodes[body_idx]->tag == "RETURN_TYPE"_) {
      returnType = ast.nodes[body_idx]->token;
      body_idx++;
    }
    auto body_ast = ast.nodes[body_idx];

    // Compile body — returns +1 Value(TAG_FUNC) wrapping a JitClosure.
    // Pass `name` so introspection (fn.name) reflects the source-level
    // identifier rather than the internal __culebra_fn_N symbol.
    auto bodyVal = compile_fn_common(&ast, *params_ast, body_ast, returnType,
                                      name);

    // Decorated fns bypass the multimethod register/install path —
    // the decorator's return value is bound directly under `name`.
    if (has_decorators) {
      llvm::Value* fnVal = bodyVal;
      for (size_t i = dec_end; i > 0; --i) {
        const auto& dec_expr = *ast.nodes[i - 1]->nodes[0];
        auto decoCallee = compile(dec_expr);
        fnVal = compile_function_call_raw(decoCallee, nullptr, {fnVal});
      }
      if (is_repl_top_level_ && is_repl_session_) {
        emit_repl_persist(fnVal, name, ".repl.decfn",
                          /*is_let=*/true, /*is_mut=*/false);
        return make_nil();
      }
      auto* existing = lookup_var(name);
      if (!existing) {
        auto slot = make_cell_slot(name, fnVal);
        define_var(name, slot);
      } else {
        store_slot(*existing, fnVal);
      }
      return make_nil();
    }

    auto bodyClosurePtr =
        builder_.CreateIntToPtr(extract_data(bodyVal), ptrTy);

    // Collect param-type annotations as a global array of C strings.
    // Only regular params participate in multifn type dispatch —
    // kw-only and `**rest` params are not part of the dispatch key.
    std::vector<llvm::Constant*> typePtrs;
    typePtrs.reserve(params_ast->nodes.size());
    for (auto& p : params_ast->nodes) {
      if (culebra::is_kw_only_sep(*p)) break;
      if (culebra::is_kwargs_rest(*p)) continue;
      auto t = extract_type_annotation(*p, 2);
      if (t.empty()) {
        typePtrs.push_back(llvm::ConstantPointerNull::get(ptrTy));
      } else {
        typePtrs.push_back(get_or_create_global_str(t, ".pt"));
      }
    }
    auto n_param_types = static_cast<int64_t>(typePtrs.size());

    llvm::Value* paramTypesPtr;
    if (typePtrs.empty()) {
      paramTypesPtr = llvm::ConstantPointerNull::get(ptrTy);
    } else {
      auto arrayTy = llvm::ArrayType::get(ptrTy, typePtrs.size());
      auto initializer = llvm::ConstantArray::get(arrayTy, typePtrs);
      auto gv = new llvm::GlobalVariable(
          *module_, arrayTy, /*isConstant=*/true,
          llvm::GlobalValue::PrivateLinkage, initializer, ".paramtypes");
      paramTypesPtr = builder_.CreateBitCast(gv, ptrTy);
    }

    auto namePtr = get_or_create_global_str(name, ".mname");
    auto dispatcherPtr = emit_call(
        module_->getOrInsertFunction(rt::multifn_register_and_install,
                                     ptrTy, ptrTy, ptrTy, ptrTy, i64Ty),
        {namePtr, bodyClosurePtr, paramTypesPtr,
         builder_.getInt64(n_param_types)},
        "multifn.disp");

    auto dispatcherVal = make_func(dispatcherPtr);

    // REPL: persist the dispatcher into session globals. Subsequent
    // `fn name(...)` decls in later inputs return the same dispatcher
    // pointer (the runtime caches by name), so repl_set's overwrite
    // semantics keep the binding stable across re-decls. +1 source:
    // `dispatcherVal` carries the +1 returned by
    // `culebra_runtime_multifn_register_and_install`; it flows
    // straight into `repl_set` and the decl returns nil, so no
    // additional retain is needed.
    if (is_repl_top_level_ && is_repl_session_) {
      emit_repl_persist(dispatcherVal, name, ".repl.multifn",
                        /*is_let=*/true, /*is_mut=*/false);
      return make_nil();
    }

    // Bind the dispatcher in the surrounding scope. First decl per name
    // creates a cell slot; subsequent decls overwrite (the runtime
    // returns the same cached dispatcher pointer, so this is a +1
    // refresh on the same object).
    auto* existing = lookup_var(name);
    if (!existing) {
      auto slot = make_cell_slot(name, dispatcherVal);
      define_var(name, slot);
    } else {
      store_slot(*existing, dispatcherVal);
    }

    return make_nil();
  }

  llvm::Value* compile_function(const peg::Ast& ast) {
    using namespace peg::udl;
    // FUNCTION: [PARAMETERS, (RETURN_TYPE)?, BLOCK]
    // METHOD:   [IDENTIFIER, PARAMETERS, BLOCK] (no return-type slot).
    const peg::Ast* params_ast;
    std::shared_ptr<peg::Ast> body_ast;
    std::string_view returnType;
    std::string_view declName;
    if (ast.tag == "METHOD"_) {
      auto mv = culebra::view_method(ast);
      declName = mv.name;
      params_ast = mv.params;
      body_ast = *mv.body;
      returnType = {};
    } else {
      auto fv = culebra::view_function(ast);
      params_ast = fv.params;
      returnType = fv.return_type;
      body_ast = fv.body;
    }
    return compile_fn_common(&ast, *params_ast, body_ast, returnType,
                              declName);
  }

  // LAMBDA ast: [LAMBDA_PARAMS, BODY]. No declared return type. BODY
  // may be any expression — compile_fn_common's generic dispatch handles
  // both BLOCK and bare expressions.
  llvm::Value* compile_lambda(const peg::Ast& ast) {
    auto fv = culebra::view_lambda(ast);
    return compile_fn_common(&ast, *fv.params, fv.body, fv.return_type, {});
  }

  // Emit the LLVM function for a FUNCTION / METHOD / synthetic-ctor AST.
  // `info_key` matches what `analyze_fn_common` used, so `func_info_`
  // lookup finds the free-var / captured-local sets.
  llvm::Value* compile_fn_common(
      const peg::Ast* info_key,
      const peg::Ast& params_ast,
      std::shared_ptr<peg::Ast> body_ast,
      std::string_view returnType,
      std::string_view declName = {}) {
    using namespace llvm;
    auto ptrTy = PointerType::get(ctx_, 0);

    // Snapshot class_type_params_ for the *immediate* function's params
    // and clear the member so nested closures (lambdas / fn inside the
    // body) don't pick up the outer class's params — matches interp's
    // neutralize_type_params, which only touches the top-level method.
    // Restored on scope exit.
    auto active_class_type_params = std::move(class_type_params_);
    class_type_params_.clear();
    struct ClassParamsGuard {
      std::vector<std::string_view>& slot;
      std::vector<std::string_view> saved;
      ~ClassParamsGuard() { slot = std::move(saved); }
    } class_params_guard{class_type_params_,
                         std::vector<std::string_view>(active_class_type_params)};

    auto infoIt = func_info_.find(info_key);
    if (infoIt == func_info_.end()) {
      throw std::runtime_error("missing func_info for function");
    }
    FuncInfo& info = infoIt->second;

    // Snapshot outer mut flags for each captured free var *before* we
    // descend into the body. emit_closure_build later re-populates this
    // (also from the outer scope), but the inner body's free-var
    // bindings consume it during their own compilation, which happens
    // first — so the snapshot must be taken here.
    info.free_var_mut.assign(info.free_vars.size(), false);
    for (size_t i = 0; i < info.free_vars.size(); i++) {
      auto outer_slot = lookup_var(info.free_vars[i]);
      if (outer_slot) info.free_var_mut[i] = outer_slot->mut;
    }

    std::vector<std::string> paramNames;
    // Owning std::string so rewrite_type_params_to_any (which returns a
    // fresh string) is safe to push; downstream consumers (emit_type_check,
    // paramTypeStrs) take string_view and accept either.
    std::vector<std::string> paramTypeNames;
    // Original declared annotation (pre-neutralization) — used by
    // introspection so `fn.params[i].type` surfaces `T` / `Array<T>`
    // for Generic class methods, not the rewritten `Any`.
    std::vector<std::string> paramDeclaredTypeNames;
    std::vector<const peg::Ast*> paramDefaults;
    std::vector<bool> paramMuts;
    std::optional<size_t> firstDefaulted;
    std::optional<size_t> kwargsRestIdx;
    std::optional<size_t> firstKwOnlyIdx;
    for (auto& node : params_ast.nodes) {
      auto pv = culebra::view_parameter(*node);
      if (pv.is_kw_only_sep) {
        if (!firstKwOnlyIdx) firstKwOnlyIdx = paramNames.size();
        continue;
      }
      if (pv.is_kwargs_rest) {
        paramNames.push_back(std::string(pv.name));
        paramTypeNames.push_back({});
        paramDeclaredTypeNames.push_back({});
        paramDefaults.push_back(nullptr);
        paramMuts.push_back(false);
        kwargsRestIdx = paramNames.size() - 1;
        continue;
      }
      paramNames.push_back(std::string(pv.name));
      auto raw = std::string(pv.type_annotation);
      paramDeclaredTypeNames.push_back(raw);
      auto tname = raw;
      // Recursive rewrite: bare `T`, `Array<T>`, `T | Long`, etc. all
      // collapse to "Any" / canonical-rewritten. Uses the snapshot
      // from the immediate enclosing class so nested fns in the body
      // don't inherit the rewrite (matches interp).
      if (!active_class_type_params.empty() && !tname.empty()) {
        tname = culebra::rewrite_type_params_to_any(
            tname, active_class_type_params);
      }
      paramTypeNames.push_back(tname);
      paramDefaults.push_back(pv.default_value);
      paramMuts.push_back(pv.is_mut);
      if (pv.default_value && !firstDefaulted) {
        firstDefaulted = paramNames.size() - 1;
      }
    }

    // Uniform closure ABI:
    //   Value fn(ptr __cls__, Value this, i64 n_args, ptr args)
    // `args` points at a caller-owned stack slab of `n_args` Value
    // structs (each with a transferred +1). Declared params and the
    // variadic `__ARGS__` binding are extracted from this slab in the
    // function prologue. The uniform shape lets every call site (user
    // calls, UFCS, runtime callbacks) use one ABI even when the
    // declared arity differs from the passed arg count.
    auto fnType = FunctionType::get(
        valueType_, {ptrTy, valueType_, builder_.getInt64Ty(), ptrTy},
        false);

    auto fnName = std::format("__culebra_fn_{}", funcCounter_++);
    auto fn = Function::Create(fnType, GlobalValue::ExternalLinkage, fnName,
                               module_);
    if (info.has_eh) {
      fn->setPersonalityFn(get_personality_fn());
    }

    auto argIt = fn->arg_begin();
    argIt->setName("__cls__");
    ++argIt;
    argIt->setName("this");
    ++argIt;
    argIt->setName("n_args");
    ++argIt;
    argIt->setName("args");

    CompilerStateSaver saver(*this);
    current_info_ = &info;
    current_return_type_ = returnType;

    auto entryBB = BasicBlock::Create(ctx_, "entry", fn);
    builder_.SetInsertPoint(entryBB);
    push_scope();

    // Function-level cleanup landingpad. Emitted only when the body
    // contains a fn-level `defer` — that's the case the JIT used to
    // skip on throw, and is the user-visible Phase 1 fix. Functions
    // without fn-level defer still rely on (a) the cycle GC for
    // throw-unwound locals and (b) per-`try`/scope landingpads for
    // localized cleanup. Making this unconditional is appealing but
    // currently destabilizes class-ctor and dispatcher emit paths
    // that synthesize closures without going through compile_fn_common.
    llvm::Value* fnMark = nullptr;
    llvm::BasicBlock* fnCleanupBB = nullptr;
    if (info.has_fn_defer) {
      fnMark = builder_.CreateCall(
          module_->getFunction(rt::defer_mark), {}, "fn.mark");
      current_fn_defer_mark_ = fnMark;
      fnCleanupBB = BasicBlock::Create(ctx_, "fn.cleanup", fn);
      current_lpad_ = fnCleanupBB;
    }

    argIt = fn->arg_begin();
    auto clsArg = &*argIt++;
    current_closure_arg_ = clsArg;
    auto thisArg = &*argIt++;
    auto nArgsArg = &*argIt++;
    auto argsArg = &*argIt++;

    // Arity guard: matching the interpreter, it is an error to call
    // with fewer args than declared. Overflow is allowed (lands in
    // `__ARGS__`). Required-arity is the number of params WITHOUT a
    // default (leading portion, since defaults must be trailing).
    size_t declaredArity = paramNames.size();
    size_t requiredArity = firstDefaulted.value_or(declaredArity);
    {
      auto need = builder_.getInt64(static_cast<int64_t>(requiredArity));
      auto tooFew = builder_.CreateICmpULT(nArgsArg, need);
      auto okBB = BasicBlock::Create(ctx_, "arity.ok", fn);
      auto errBB = BasicBlock::Create(ctx_, "arity.err", fn);
      builder_.CreateCondBr(tooFew, errBB, okBB);

      builder_.SetInsertPoint(errBB);
      // Build a constant char* array of declared param names so the
      // runtime can name the first missing slot ("missing required
      // argument 'X'"). Matches interp's bind_call_args message.
      std::vector<llvm::Constant*> nameConsts;
      nameConsts.reserve(paramNames.size());
      for (const auto& n : paramNames) {
        nameConsts.push_back(llvm::cast<llvm::Constant>(
            get_or_create_global_str(n, ".paramname")));
      }
      auto nameArrayTy = llvm::ArrayType::get(ptrTy, nameConsts.size());
      auto nameArrayConst = llvm::ConstantArray::get(nameArrayTy, nameConsts);
      auto namesGlobal = new llvm::GlobalVariable(
          *module_, nameArrayTy, /*isConstant=*/true,
          llvm::GlobalValue::PrivateLinkage, nameArrayConst, ".paramnames");
      emit_call(
          module_->getOrInsertFunction(
              rt::arity_missing, builder_.getVoidTy(),
              ptrTy, builder_.getInt64Ty(),
              builder_.getInt64Ty(), builder_.getInt64Ty()),
          {namesGlobal, nArgsArg,
           current_line_val(), current_column_val()});
      builder_.CreateUnreachable();
      builder_.SetInsertPoint(okBB);
    }

    // `self` = make_func(__cls__): calling it re-enters this fn with this
    // closure. make_func doesn't retain, so retain explicitly so the
    // owned `self` slot has the +1 it will hand back at function exit.
    {
      auto selfVal = make_func(clsArg);
      emit_value_retain(selfVal);
      define_var("self", make_stack_slot("self", selfVal));
    }

    // `this` is from arg; if captured, allocate cell. The arg's +1 was
    // pre-retained by the caller (via load/compile of the receiver) and
    // is transferred into the slot — no extra retain needed.
    if (info.captured_locals.contains("this")) {
      define_var("this", make_cell_slot("this", thisArg));
    } else {
      define_var("this", make_stack_slot("this", thisArg));
    }

    // Free variables bound BEFORE params so that default expressions on
    // parameters can reference captured outer variables. These are
    // borrowed cell refs — the closure object owns them.
    for (size_t i = 0; i < info.free_vars.size(); i++) {
      const auto& fv = info.free_vars[i];
      auto capturesFieldPtr =
          builder_.CreateStructGEP(closureType_, clsArg, 3, "caps.ptr");
      auto capturesArr =
          builder_.CreateLoad(ptrTy, capturesFieldPtr, "caps");
      auto cellSlotPtr = builder_.CreateInBoundsGEP(
          ptrTy, capturesArr, {builder_.getInt64(i)}, "cell.slot");
      auto cellPtr = builder_.CreateLoad(ptrTy, cellSlotPtr, fv + ".cell");
      llvm::IRBuilder<> entryB(entryBB, entryBB->begin());
      auto holder = entryB.CreateAlloca(ptrTy, nullptr, fv);
      builder_.CreateStore(cellPtr, holder);
      bool fv_mut = i < info.free_var_mut.size() ? info.free_var_mut[i] : false;
      define_var(fv, VarSlot{VarSlot::Cell, holder, /*owned=*/false, fv_mut});
    }

    // Declared parameters: for non-defaulted params, load from args[i]
    // (the caller transferred each +1 into the slab). For defaulted
    // params, branch on `i < n_args && slab[i].tag != TAG_UNFILLED`:
    // either take the slab entry or compile the default expression
    // inline, then PHI-merge. TAG_UNFILLED is the kwargs-resolver's
    // middle-gap sentinel.
    for (size_t i = 0; i < paramNames.size(); i++) {
      const auto& name = paramNames[i];
      llvm::Value* argVal = nullptr;
      if (!paramDefaults[i]) {
        auto slotPtr = builder_.CreateInBoundsGEP(
            valueType_, argsArg, {builder_.getInt64(static_cast<int64_t>(i))},
            name + ".slot");
        argVal = builder_.CreateLoad(valueType_, slotPtr, name);
      } else {
        auto hasIdx = builder_.CreateICmpUGT(
            nArgsArg, builder_.getInt64(static_cast<int64_t>(i)),
            name + ".has");
        auto takeBB = BasicBlock::Create(ctx_, name + ".take", fn);
        auto checkBB = BasicBlock::Create(ctx_, name + ".check", fn);
        auto defBB = BasicBlock::Create(ctx_, name + ".def", fn);
        auto mergeBB = BasicBlock::Create(ctx_, name + ".merge", fn);
        builder_.CreateCondBr(hasIdx, checkBB, defBB);

        builder_.SetInsertPoint(checkBB);
        auto slotPtr = builder_.CreateInBoundsGEP(
            valueType_, argsArg, {builder_.getInt64(static_cast<int64_t>(i))},
            name + ".slot");
        // Load tag first; if TAG_UNFILLED, fall through to default. The
        // value-load happens after the tag check so we never observe
        // the sentinel's data.
        auto slotTag = builder_.CreateLoad(builder_.getInt8Ty(),
                                            slotPtr, name + ".tag");
        auto isUnfilled = builder_.CreateICmpEQ(
            slotTag, builder_.getInt8(TAG_UNFILLED), name + ".unf");
        builder_.CreateCondBr(isUnfilled, defBB, takeBB);

        builder_.SetInsertPoint(takeBB);
        auto fromArgs = builder_.CreateLoad(valueType_, slotPtr, name);
        auto takeEndBB = builder_.GetInsertBlock();
        builder_.CreateBr(mergeBB);

        builder_.SetInsertPoint(defBB);
        auto defVal = compile(*paramDefaults[i]);
        emit_value_retain(defVal);  // match caller's +1 transfer discipline
        auto defEndBB = builder_.GetInsertBlock();
        builder_.CreateBr(mergeBB);

        builder_.SetInsertPoint(mergeBB);
        auto phi = builder_.CreatePHI(valueType_, 2, name + ".phi");
        phi->addIncoming(fromArgs, takeEndBB);
        phi->addIncoming(defVal, defEndBB);
        argVal = phi;
      }
      if (!paramTypeNames[i].empty()) {
        emit_type_check(argVal, paramTypeNames[i],
                        std::string("parameter '") + name + "'");
      }
      if (is_sink_name(name)) {
        // `fn(_, _, x)`: drop each `_` arg's +1 transfer instead of
        // binding it. Allows repeated `_` params without slot collision.
        emit_value_release(argVal);
      } else if (info.captured_locals.contains(name)) {
        define_var(name, make_cell_slot(name, argVal, /*is_mut=*/paramMuts[i]));
      } else {
        define_var(name, make_stack_slot(name, argVal, /*is_mut=*/paramMuts[i]));
      }
    }

    // __ARGS__: Array of overflow args (args[declaredArity..n_args)).
    // Built only when the body references it (FuncInfo::uses_args).
    // Otherwise we still need to release the +1 retains the caller
    // transferred for each overflow slot — but the dedicated helper
    // (release_overflow_args) skips the Array allocation entirely.
    if (info.uses_args) {
      auto argsArr = emit_call(
          module_->getOrInsertFunction(
              rt::args_slice_to_array, ptrTy, ptrTy,
              builder_.getInt64Ty(), builder_.getInt64Ty()),
          {argsArg,
           builder_.getInt64(static_cast<int64_t>(declaredArity)),
           nArgsArg},
          "args.arr");
      auto argsVal = make_array(argsArr);
      if (info.captured_locals.contains("__ARGS__")) {
        define_var("__ARGS__", make_cell_slot("__ARGS__", argsVal));
      } else {
        define_var("__ARGS__", make_stack_slot("__ARGS__", argsVal));
      }
    } else {
      // Conditional: only call the release helper when there *are*
      // overflow args (n_args > declaredArity). For the typical case
      // where the caller passes the declared count exactly, this
      // collapses to a single compare + not-taken branch — cheaper
      // than always entering the helper.
      auto fn = builder_.GetInsertBlock()->getParent();
      auto needRelBB = llvm::BasicBlock::Create(ctx_, "args.release", fn);
      auto skipBB = llvm::BasicBlock::Create(ctx_, "args.no_release", fn);
      auto declI64 =
          builder_.getInt64(static_cast<int64_t>(declaredArity));
      auto hasOverflow = builder_.CreateICmpUGT(nArgsArg, declI64,
                                                "args.has_overflow");
      builder_.CreateCondBr(hasOverflow, needRelBB, skipBB);
      builder_.SetInsertPoint(needRelBB);
      emit_call(
          module_->getOrInsertFunction(
              rt::release_overflow_args, builder_.getVoidTy(), ptrTy,
              builder_.getInt64Ty(), builder_.getInt64Ty()),
          {argsArg, declI64, nArgsArg});
      builder_.CreateBr(skipBB);
      builder_.SetInsertPoint(skipBB);
    }

    // Forward-reference support: closure capture happens lazily through
    // pre-allocated cells. See `pre_allocate_forward_refs` doc.
    pre_allocate_forward_refs(*body_ast);

    auto bodyVal = compile(*body_ast);

    if (!builder_.GetInsertBlock()->getTerminator()) {
      if (!returnType.empty()) {
        emit_type_check(bodyVal, returnType, "return value");
      }
      // Run function-level defers before returning.
      if (fnMark) {
        builder_.CreateCall(
            module_->getFunction(rt::defer_run_to), {fnMark});
      }
      release_all_scopes_for_exit();
      builder_.CreateRet(bodyVal);
    }

    // Throw-path cleanup landingpad for the function frame. Releases
    // the owned slots (params / locals are zero-init'd in entry so
    // pre-init throws release nil, a no-op), runs any fn-level defers
    // back to `fn.mark`, then rethrows. This pairs with the
    // `current_lpad_ = fnCleanupBB` wired at function entry so every
    // throw-capable invoke in the body unwinds through here.
    if (fnCleanupBB) {
      auto ptrTy = PointerType::get(ctx_, 0);
      builder_.SetInsertPoint(fnCleanupBB);
      auto lpadTy =
          llvm::StructType::get(ptrTy, builder_.getInt32Ty());
      auto lpad =
          builder_.CreateLandingPad(lpadTy, 1, "fn.exc");
      lpad->addClause(llvm::ConstantPointerNull::get(ptrTy));
      auto excPtr = builder_.CreateExtractValue(lpad, {0});
      builder_.CreateCall(
          module_->getOrInsertFunction(
              "__cxa_begin_catch", ptrTy, ptrTy),
          {excPtr});
      if (fnMark) {
        builder_.CreateCall(
            module_->getFunction(rt::defer_run_to), {fnMark});
      }
      release_all_scopes_for_exit();
      emit_rethrow(/*outerLpad=*/nullptr);
    }

    pop_scope();
    verifyFunction(*fn);

    // Restore outer context before emitting the closure at the caller's
    // insertion point — the cell captures come from the outer scope.
    saver.restore();
    auto* paramMeta = emit_param_meta_global(
        fn, paramNames, paramDefaults, kwargsRestIdx, firstKwOnlyIdx,
        std::string(declName), std::string(returnType), paramMuts,
        paramTypeNames, paramDeclaredTypeNames);
    return emit_closure_build(fn, info, paramNames.size(), paramMeta);
  }

  // Emit module-level globals describing this function's parameter
  // list, then return a constant pointer to a JitParamMeta struct. The
  // runtime resolver `culebra_runtime_call_with_kwargs` consults this
  // table via the side map keyed by `fn_ptr`. Returns nullptr when
  // there are no params — built-in closures and zero-arg functions
  // skip metadata entirely (kwargs against them error cleanly).
  llvm::Constant* emit_param_meta_global(
      llvm::Function* fn,
      const std::vector<std::string>& paramNames,
      const std::vector<const peg::Ast*>& paramDefaults,
      std::optional<size_t> kwargsRestIdx = std::nullopt,
      std::optional<size_t> firstKwOnlyIdx = std::nullopt,
      const std::string& fnName = {},
      const std::string& returnType = {},
      const std::vector<bool>& paramMuts = {},
      const std::vector<std::string>& paramTypes = {},
      const std::vector<std::string>& paramDeclaredTypes = {}) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i64Ty = builder_.getInt64Ty();
    auto i8Ty = builder_.getInt8Ty();
    auto fnBase = std::string(fn->getName());

    // Nullary functions still need a meta global so fn.name and
    // fn.return_type can be read by introspection. Emit a minimal
    // meta with null param-array pointers and n_params=0; the
    // runtime helper's loops skip element access when n_params==0.
    if (paramNames.empty()) {
      auto fnNameG = builder_.CreateGlobalString(
          fnName, fnBase + ".pmeta.fn");
      auto retTyG = builder_.CreateGlobalString(
          returnType, fnBase + ".pmeta.ret");
      auto metaTy = llvm::StructType::get(ctx_,
          {ptrTy, ptrTy, i64Ty, i64Ty, i64Ty,
           ptrTy, ptrTy, ptrTy, ptrTy, ptrTy});
      auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);
      auto metaInit = llvm::ConstantStruct::get(
          metaTy,
          {nullPtr, nullPtr,
           llvm::ConstantInt::get(i64Ty, 0),
           llvm::ConstantInt::get(i64Ty, -1),
           llvm::ConstantInt::get(i64Ty, -1),
           llvm::ConstantExpr::getBitCast(fnNameG, ptrTy),
           llvm::ConstantExpr::getBitCast(retTyG, ptrTy),
           nullPtr, nullPtr, nullPtr});
      auto metaGlobal = new llvm::GlobalVariable(
          *module_, metaTy, /*isConstant=*/true,
          llvm::GlobalValue::PrivateLinkage, metaInit,
          fnBase + ".pmeta");
      return metaGlobal;
    }

    // Names array: one cstring per param.
    std::vector<llvm::Constant*> name_consts;
    name_consts.reserve(paramNames.size());
    for (const auto& nm : paramNames) {
      name_consts.push_back(builder_.CreateGlobalString(
          nm, ".param.name." + fnBase + "." + nm));
    }
    auto namesArrTy = llvm::ArrayType::get(ptrTy, name_consts.size());
    auto namesInit = llvm::ConstantArray::get(namesArrTy, name_consts);
    auto namesGlobal = new llvm::GlobalVariable(
        *module_, namesArrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, namesInit,
        fnBase + ".pmeta.names");

    // has_default bitmask: ceil(N/8) bytes, bit i = paramDefaults[i].
    size_t n_bytes = (paramNames.size() + 7) / 8;
    std::vector<llvm::Constant*> bit_consts(n_bytes,
                                             builder_.getInt8(0));
    for (size_t i = 0; i < paramDefaults.size(); i++) {
      if (paramDefaults[i]) {
        auto byte_idx = i / 8;
        auto cur = llvm::cast<llvm::ConstantInt>(bit_consts[byte_idx])
                       ->getZExtValue();
        bit_consts[byte_idx] =
            builder_.getInt8(cur | (1u << (i % 8)));
      }
    }
    auto bitsArrTy = llvm::ArrayType::get(i8Ty, n_bytes);
    auto bitsInit = llvm::ConstantArray::get(bitsArrTy, bit_consts);
    auto bitsGlobal = new llvm::GlobalVariable(
        *module_, bitsArrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, bitsInit,
        fnBase + ".pmeta.bits");

    // Introspection-only globals (fn name, return type, param mut bits,
    // param type names). Empty strings replace nullptr so the runtime
    // can always deref these without a null check.
    auto fnNameG = builder_.CreateGlobalString(fnName, fnBase + ".pmeta.fn");
    auto retTyG = builder_.CreateGlobalString(
        returnType, fnBase + ".pmeta.ret");

    std::vector<llvm::Constant*> mut_consts(n_bytes, builder_.getInt8(0));
    for (size_t i = 0; i < paramMuts.size(); i++) {
      if (paramMuts[i]) {
        auto byte_idx = i / 8;
        auto cur = llvm::cast<llvm::ConstantInt>(mut_consts[byte_idx])
                       ->getZExtValue();
        mut_consts[byte_idx] = builder_.getInt8(cur | (1u << (i % 8)));
      }
    }
    auto mutBitsInit = llvm::ConstantArray::get(bitsArrTy, mut_consts);
    auto mutBitsGlobal = new llvm::GlobalVariable(
        *module_, bitsArrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, mutBitsInit,
        fnBase + ".pmeta.muts");

    std::vector<llvm::Constant*> type_consts;
    type_consts.reserve(paramNames.size());
    for (size_t i = 0; i < paramNames.size(); i++) {
      const std::string& t = i < paramTypes.size() ? paramTypes[i] : "";
      type_consts.push_back(builder_.CreateGlobalString(
          t, ".param.type." + fnBase + "." + paramNames[i]));
    }
    auto typesArrTy = llvm::ArrayType::get(ptrTy, type_consts.size());
    auto typesInit = llvm::ConstantArray::get(typesArrTy, type_consts);
    auto typesGlobal = new llvm::GlobalVariable(
        *module_, typesArrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, typesInit,
        fnBase + ".pmeta.types");

    // Declared (pre-neutralization) types for introspection — same
    // layout as types, falls back to the effective type when missing.
    std::vector<llvm::Constant*> declared_consts;
    declared_consts.reserve(paramNames.size());
    for (size_t i = 0; i < paramNames.size(); i++) {
      const std::string& t = i < paramDeclaredTypes.size() &&
                                 !paramDeclaredTypes[i].empty()
                                 ? paramDeclaredTypes[i]
                                 : (i < paramTypes.size() ? paramTypes[i]
                                                          : std::string());
      declared_consts.push_back(builder_.CreateGlobalString(
          t, ".param.declared." + fnBase + "." + paramNames[i]));
    }
    auto declaredArrTy = llvm::ArrayType::get(ptrTy, declared_consts.size());
    auto declaredInit = llvm::ConstantArray::get(declaredArrTy, declared_consts);
    auto declaredGlobal = new llvm::GlobalVariable(
        *module_, declaredArrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, declaredInit,
        fnBase + ".pmeta.declared");

    // Layout matches JitParamMeta in declaration order — append new
    // fields at the end so kwargs dispatch consumers stay unaffected.
    auto metaTy = llvm::StructType::get(ctx_,
        {ptrTy, ptrTy, i64Ty, i64Ty, i64Ty,
         ptrTy, ptrTy, ptrTy, ptrTy, ptrTy});
    auto metaInit = llvm::ConstantStruct::get(
        metaTy,
        {llvm::ConstantExpr::getBitCast(namesGlobal, ptrTy),
         llvm::ConstantExpr::getBitCast(bitsGlobal, ptrTy),
         llvm::ConstantInt::get(i64Ty,
                                static_cast<int64_t>(paramNames.size())),
         llvm::ConstantInt::get(i64Ty,
             kwargsRestIdx ? static_cast<int64_t>(*kwargsRestIdx) : -1),
         llvm::ConstantInt::get(i64Ty,
             firstKwOnlyIdx ? static_cast<int64_t>(*firstKwOnlyIdx) : -1),
         llvm::ConstantExpr::getBitCast(fnNameG, ptrTy),
         llvm::ConstantExpr::getBitCast(retTyG, ptrTy),
         llvm::ConstantExpr::getBitCast(mutBitsGlobal, ptrTy),
         llvm::ConstantExpr::getBitCast(typesGlobal, ptrTy),
         llvm::ConstantExpr::getBitCast(declaredGlobal, ptrTy)});
    auto metaGlobal = new llvm::GlobalVariable(
        *module_, metaTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, metaInit,
        fnBase + ".pmeta");
    return metaGlobal;
  }

  // Construct a JitClosure for `fn` at the current insertion point:
  // calls `closure_new`, then fills in the captures array with cell
  // pointers from the caller's scope (each retained). Returns the
  // closure as a %Value (TAG_FUNC). Shared by compile_function and
  // compile_defer (defer uses arity=0).
  llvm::Value* emit_closure_build(llvm::Function* fn, FuncInfo& info,
                                  size_t arity,
                                  llvm::Constant* paramMeta = nullptr) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto n = info.free_vars.size();
    auto closurePtr = emit_call(
        module_->getOrInsertFunction(rt::closure_new, ptrTy,
                                     ptrTy, builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {fn, builder_.getInt64(n), builder_.getInt64(arity)}, "closure");
    // Register `fn` → param metadata once per closure instantiation.
    // The registration is idempotent (overwrites the same key with the
    // same pointer), so calling it on every `let f = fn` execution is
    // cheap; the kwargs resolver looks it up by `fn_ptr` at call time.
    if (paramMeta) {
      emit_call(
          module_->getOrInsertFunction(rt::register_param_meta,
                                       builder_.getVoidTy(), ptrTy, ptrTy),
          {fn, paramMeta});
    }
    info.free_var_mut.assign(n, false);
    if (n > 0) {
      auto capturesFieldPtr =
          builder_.CreateStructGEP(closureType_, closurePtr, 3);
      auto capturesArr = builder_.CreateLoad(ptrTy, capturesFieldPtr);
      for (size_t i = 0; i < n; i++) {
        const auto& fv = info.free_vars[i];
        auto slot = lookup_var(fv);
        if (!slot) {
          throw std::runtime_error(
              std::format("cannot find free var '{}' in scope", fv));
        }
        if (slot->kind != VarSlot::Cell) {
          throw std::runtime_error(
              std::format("free var '{}' is not a cell", fv));
        }
        info.free_var_mut[i] = slot->mut;
        auto cellPtr = cell_ptr_of(*slot);
        auto dstSlot = builder_.CreateInBoundsGEP(
            ptrTy, capturesArr, {builder_.getInt64(i)});
        builder_.CreateStore(cellPtr, dstSlot);
        emit_cell_retain(cellPtr);  // closure owns a ref to each cell
      }
    }
    return make_func(closurePtr);
  }

  // --- Call ---

  llvm::Value* compile_call(const peg::Ast& ast) {
    using namespace peg::udl;
    auto calleeNode = ast.nodes[0];
    llvm::Value* callee = compile(*calleeNode);

    for (auto i = 1u; i < ast.nodes.size(); i++) {
      const auto& postfix = *ast.nodes[i];

      switch (postfix.original_tag) {
        case "ARGUMENTS"_: {
          // First postfix on an `f(x, y: 2)`-style call: if `f` is an
          // IDENTIFIER bound directly to a `fn (...) {...}` literal in
          // scope AND every splat is a literal Object, route through
          // the compile-time resolver (zero runtime overhead). Any
          // dynamic splat or indirect callee falls back to the
          // runtime resolver via `compile_function_call`.
          const peg::Ast* fnAst = nullptr;
          if (i == 1 && calleeNode->tag == "IDENTIFIER"_) {
            fnAst = lookup_fn_ast(std::string(calleeNode->token));
          }
          bool has_dynamic_splat = false;
          bool fn_has_kw_marker = false;
          if (fnAst) {
            for (auto& c : postfix.nodes) {
              if (c->tag == "KWARG_SPLAT"_ &&
                  c->nodes[0]->tag != "OBJECT"_) {
                has_dynamic_splat = true;
                break;
              }
            }
            for (auto& p : fnAst->nodes[0]->nodes) {
              if (culebra::is_kw_only_sep(*p) ||
                  culebra::is_kwargs_rest(*p)) {
                fn_has_kw_marker = true;
                break;
              }
            }
          }
          bool need_kwarg_path =
              !arg_list_is_positional_only(postfix) || fn_has_kw_marker;
          if (fnAst && !has_dynamic_splat && need_kwarg_path) {
            callee = compile_function_call_with_kwargs(
                postfix, callee, *fnAst);
          } else {
            callee = compile_function_call(postfix, callee);
          }
          break;
        }
        case "INDEX"_: {
          // INDEX/DOT return borrowed slot values; promote them to +1
          // so chained access like `a.b[i].c` doesn't over-release the
          // intermediate when the next step's swap drops it.
          auto receiver = callee;
          callee = compile_index_access(postfix, receiver);
          emit_value_swap_owned(callee, receiver);
          break;
        }
        case "DOT"_: {
          if (i + 1 < ast.nodes.size() &&
              ast.nodes[i + 1]->original_tag == "ARGUMENTS"_) {
            auto method = std::string(postfix.token);
            // The lazy iterator path allocates a wrapping iterator and
            // walks it via per-element runtime closure calls; fusing
            // `.map(λ).collect()` collapses both into one inline loop.
            if (method == "map") {
              if (auto fused = try_fuse_iter_map_collect(ast, i, callee)) {
                callee = *fused;
                i += 3;  // consume ARGUMENTS, DOT(collect), ARGUMENTS
                break;
              }
            }
            callee = compile_method_call(method, *ast.nodes[i + 1], callee);
            i++;  // consume ARGUMENTS
          } else {
            auto name = std::string(postfix.token);
            auto receiver = callee;
            callee = compile_property_get(receiver, name);
            // Function introspection (.name / .params / .return_type)
            // returns a fresh +1 owned value rather than a borrowed
            // Object IC slot view, so swap_owned would double-retain it
            // and leak. Release the receiver directly instead.
            if (name == "name" || name == "params" || name == "return_type") {
              emit_value_release(receiver);
            } else {
              emit_value_swap_owned(callee, receiver);
            }
          }
          break;
        }
        default:
          throw std::runtime_error("invalid call postfix");
      }
    }

    return callee;
  }

  // Get a property from an object (TAG_OBJECT required).
  //
  // Inlines a V8/SpiderMonkey-style monomorphic inline cache: each call
  // site owns a private IC global of `{Shape*, slot_offset}`. Fast path
  // (no runtime call): load `obj->shape`, compare with the cached
  // shape, and on hit load `slots.data()[offset].value` directly. Slow
  // path: call `culebra_runtime_object_get_ic`, which looks up the
  // shape and refreshes the IC. Layout assumption: std::vector's first
  // member is its data pointer (true on libc++/libstdc++/MSVC STL).
  llvm::Value* compile_property_get(llvm::Value* receiver,
                                    const std::string& name) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i8Ty = builder_.getInt8Ty();
    auto i64Ty = builder_.getInt64Ty();

    auto tag = extract_tag(receiver);
    auto fn = builder_.GetInsertBlock()->getParent();

    // Function introspection: `.name` / `.params` / `.return_type` on a
    // function value follow a separate runtime path. Object IC stays
    // intact for receivers tagged TAG_OBJECT (a user-defined property
    // named `name` still resolves through the IC).
    bool fn_mode =
        name == "name" || name == "params" || name == "return_type";
    llvm::BasicBlock* finalMergeBB = nullptr;
    llvm::BasicBlock* fnEnd = nullptr;
    llvm::Value* introResult = nullptr;
    if (fn_mode) {
      auto isFn =
          builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_FUNC), "is.fn");
      auto fnBB = llvm::BasicBlock::Create(ctx_, "prop.fn", fn);
      auto notFnBB = llvm::BasicBlock::Create(ctx_, "prop.not_fn", fn);
      finalMergeBB = llvm::BasicBlock::Create(ctx_, "prop.final", fn);
      builder_.CreateCondBr(isFn, fnBB, notFnBB);

      builder_.SetInsertPoint(fnBB);
      auto clsPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
      auto keyPtr = builder_.CreateGlobalString(name, ".fn.prop");
      introResult = emit_call(
          module_->getOrInsertFunction(rt::fn_introspect_get,
                                       valueType_, ptrTy, ptrTy),
          {clsPtr, keyPtr}, "fn.intro");
      // Caller (compile_call_with_builtins) runs swap_owned on the
      // returned value, which retains `introResult` and releases the
      // original receiver. The Object path leaves receiver alive too —
      // mirror that contract here.
      fnEnd = builder_.GetInsertBlock();
      builder_.CreateBr(finalMergeBB);

      builder_.SetInsertPoint(notFnBB);
    }

    auto isObj =
        builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT), "is.obj");
    auto okBB = llvm::BasicBlock::Create(ctx_, "prop.ok", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "prop.err", fn);
    builder_.CreateCondBr(isObj, okBB, errBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("Object", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(okBB);
    auto objPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto keyPtr = builder_.CreateGlobalString(name, ".key");

    // Initialise expected_shape to a non-null sentinel `(void*)1` so
    // the very first call always misses to slow path. Real Shape*
    // pointers are heap-allocated and 8-byte aligned, never == 1, so
    // the sentinel cannot collide with a legitimate shape (in
    // particular, it does NOT match the `shape == nullptr` state of a
    // freshly-allocated Object — which the prior nullptr initialiser
    // would have spuriously fast-pathed into an OOB slots[0] read).
    auto icTy = llvm::StructType::get(ctx_, {ptrTy, i64Ty});
    auto* sentinelPtr = llvm::ConstantExpr::getIntToPtr(
        llvm::ConstantInt::get(i64Ty, 1), ptrTy);
    auto* icInit = llvm::ConstantStruct::get(
        icTy, {sentinelPtr, llvm::ConstantInt::get(i64Ty, 0)});
    auto* icGlobal = new llvm::GlobalVariable(
        *module_, icTy, /*isConstant=*/false,
        llvm::GlobalValue::PrivateLinkage, icInit,
        ".prop.ic." + std::to_string(prop_ic_counter_++));

    auto fastBB = llvm::BasicBlock::Create(ctx_, "prop.fast", fn);
    auto slowBB = llvm::BasicBlock::Create(ctx_, "prop.slow", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "prop.merge", fn);

    auto shapeFieldPtr = builder_.CreateConstInBoundsGEP1_64(
        i8Ty, objPtr, offsetof(JitObject, shape), "shape.fieldp");
    auto objShape = builder_.CreateLoad(ptrTy, shapeFieldPtr, "obj.shape");
    auto icShapePtr =
        builder_.CreateStructGEP(icTy, icGlobal, 0, "ic.shape.p");
    auto icShape = builder_.CreateLoad(ptrTy, icShapePtr, "ic.shape");
    auto shapeMatch =
        builder_.CreateICmpEQ(objShape, icShape, "shape.match");
    builder_.CreateCondBr(shapeMatch, fastBB, slowBB);

    builder_.SetInsertPoint(fastBB);
    auto icOffsetPtr =
        builder_.CreateStructGEP(icTy, icGlobal, 1, "ic.off.p");
    auto icOffset = builder_.CreateLoad(i64Ty, icOffsetPtr, "ic.off");

    auto slotsFieldPtr = builder_.CreateConstInBoundsGEP1_64(
        i8Ty, objPtr, offsetof(JitObject, slots), "slots.vec.p");
    auto slotsData = builder_.CreateLoad(ptrTy, slotsFieldPtr, "slots.data");

    auto byteOffset = builder_.CreateMul(
        icOffset, llvm::ConstantInt::get(i64Ty, sizeof(JitObjectEntry)),
        "entry.byte.off");
    auto entryPtr = builder_.CreateInBoundsGEP(
        i8Ty, slotsData, byteOffset, "entry.p");

    auto fastTag = builder_.CreateLoad(i8Ty, entryPtr, "fast.tag");
    auto entryDataPtr = builder_.CreateConstInBoundsGEP1_64(
        i8Ty, entryPtr, offsetof(JitValue, data), "entry.data.p");
    auto fastData = builder_.CreateLoad(i64Ty, entryDataPtr, "fast.data");
    builder_.CreateBr(mergeBB);
    auto fastEnd = builder_.GetInsertBlock();

    builder_.SetInsertPoint(slowBB);
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto outTag = entryB.CreateAlloca(i8Ty, nullptr, "get.tag");
    auto outData = entryB.CreateAlloca(i64Ty, nullptr, "get.data");
    emit_call(
        module_->getOrInsertFunction(rt::object_get_ic,
                                     builder_.getVoidTy(), ptrTy, ptrTy,
                                     ptrTy, ptrTy, ptrTy),
        {objPtr, keyPtr, icGlobal, outTag, outData});
    auto slowTag = builder_.CreateLoad(i8Ty, outTag, "slow.tag");
    auto slowData = builder_.CreateLoad(i64Ty, outData, "slow.data");
    builder_.CreateBr(mergeBB);
    auto slowEnd = builder_.GetInsertBlock();

    builder_.SetInsertPoint(mergeBB);
    auto tagPhi = builder_.CreatePHI(i8Ty, 2, "prop.tag");
    tagPhi->addIncoming(fastTag, fastEnd);
    tagPhi->addIncoming(slowTag, slowEnd);
    auto dataPhi = builder_.CreatePHI(i64Ty, 2, "prop.data");
    dataPhi->addIncoming(fastData, fastEnd);
    dataPhi->addIncoming(slowData, slowEnd);

    llvm::Value* result = llvm::UndefValue::get(valueType_);
    result = builder_.CreateInsertValue(result, tagPhi, {0});
    result = builder_.CreateInsertValue(result, dataPhi, {1});

    // Merge with the function-introspection branch when active.
    if (fn_mode) {
      auto objEnd = builder_.GetInsertBlock();
      builder_.CreateBr(finalMergeBB);
      builder_.SetInsertPoint(finalMergeBB);
      auto finalPhi = builder_.CreatePHI(valueType_, 2, "prop.result");
      finalPhi->addIncoming(introResult, fnEnd);
      finalPhi->addIncoming(result, objEnd);
      return finalPhi;
    }
    return result;
  }

  llvm::Value* compile_index_access(const peg::Ast& idxAst,
                                    llvm::Value* arr) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i8Ty = builder_.getInt8Ty();
    auto i64Ty = builder_.getInt64Ty();
    auto fn = builder_.GetInsertBlock()->getParent();
    auto tag = extract_tag(arr);

    auto arrBB    = llvm::BasicBlock::Create(ctx_, "idx.arr", fn);
    auto objBB    = llvm::BasicBlock::Create(ctx_, "idx.obj", fn);
    auto errBB    = llvm::BasicBlock::Create(ctx_, "idx.err", fn);
    auto mergeBB  = llvm::BasicBlock::Create(ctx_, "idx.merge", fn);

    // Both Array and Tuple share JitArray storage and the array_get
    // runtime helper indexes by Long for either.
    auto isArr = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_ARRAY));
    auto chkTupBB = llvm::BasicBlock::Create(ctx_, "idx.chk_tup", fn);
    auto chkObjBB = llvm::BasicBlock::Create(ctx_, "idx.chk_obj", fn);
    builder_.CreateCondBr(isArr, arrBB, chkTupBB);
    builder_.SetInsertPoint(chkTupBB);
    auto isTup = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_TUPLE));
    builder_.CreateCondBr(isTup, arrBB, chkObjBB);
    builder_.SetInsertPoint(chkObjBB);
    auto isObj = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));
    builder_.CreateCondBr(isObj, objBB, errBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("Array", tag);
    builder_.CreateUnreachable();

    // Allocate output slots in entry block; reused by both paths.
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto outTag = entryB.CreateAlloca(i8Ty, nullptr, "idx.out.tag");
    auto outData = entryB.CreateAlloca(i64Ty, nullptr, "idx.out.data");

    // Array path: existing behavior — index by Long.
    builder_.SetInsertPoint(arrBB);
    auto arrPtr = builder_.CreateIntToPtr(extract_data(arr), ptrTy);
    auto idxVal = compile(idxAst);
    auto idx = value_to_long(idxVal);
    emit_call(
        module_->getOrInsertFunction(
            rt::array_get, builder_.getVoidTy(), ptrTy, i64Ty, ptrTy,
            ptrTy, i64Ty, i64Ty),
        {arrPtr, idx, outTag, outData, current_line_val(),
         current_column_val()});
    builder_.CreateBr(mergeBB);

    // Object path: look up by Value key in the non-String sidecar.
    builder_.SetInsertPoint(objBB);
    auto objPtr = builder_.CreateIntToPtr(extract_data(arr), ptrTy);
    auto objKey = compile(idxAst);
    emit_call(
        module_->getOrInsertFunction(
            rt::object_get_any, builder_.getVoidTy(), ptrTy, i8Ty, i64Ty,
            ptrTy, ptrTy),
        {objPtr, extract_tag(objKey), extract_data(objKey), outTag, outData});
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto tagLoaded = builder_.CreateLoad(i8Ty, outTag);
    auto dataLoaded = builder_.CreateLoad(i64Ty, outData);
    llvm::Value* result = llvm::UndefValue::get(valueType_);
    result = builder_.CreateInsertValue(result, tagLoaded, {0});
    result = builder_.CreateInsertValue(result, dataLoaded, {1});
    return result;
  }

  // Helper: check receiver tag matches expected TAG_*, else type error.
  llvm::Value* expect_tag(llvm::Value* receiver, int8_t expected,
                          const char* bb_prefix) {
    auto tag = extract_tag(receiver);
    auto fn = builder_.GetInsertBlock()->getParent();
    auto okBB = llvm::BasicBlock::Create(
        ctx_, std::string(bb_prefix) + ".ok", fn);
    auto errBB = llvm::BasicBlock::Create(
        ctx_, std::string(bb_prefix) + ".err", fn);
    auto cond = builder_.CreateICmpEQ(tag, builder_.getInt8(expected));
    builder_.CreateCondBr(cond, okBB, errBB);
    builder_.SetInsertPoint(errBB);
    emit_type_error_typed(_culebra_tag_name(expected), tag);
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(okBB);
    return builder_.CreateIntToPtr(extract_data(receiver),
                                   llvm::PointerType::get(ctx_, 0));
  }

  // Coerce a String/StringView receiver to a null-terminated cstr.
  // StringView is materialized via strlike_to_cstr (leak-bounded).
  llvm::Value* coerce_strlike_cstr(llvm::Value* val, const char* bb_prefix) {
    auto tag = extract_tag(val);
    auto fn = builder_.GetInsertBlock()->getParent();
    auto okBB = llvm::BasicBlock::Create(
        ctx_, std::string(bb_prefix) + ".ok", fn);
    auto errBB = llvm::BasicBlock::Create(
        ctx_, std::string(bb_prefix) + ".err", fn);
    auto isStr = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_STRING));
    auto isView = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_STRINGVIEW));
    auto cond = builder_.CreateOr(isStr, isView);
    builder_.CreateCondBr(cond, okBB, errBB);
    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("String", tag);
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(okBB);
    return emit_call(module_->getFunction(rt::strlike_to_cstr),
                     {tag, extract_data(val)});
  }

  llvm::Value* compile_method_call(const std::string& method,
                                   const peg::Ast& argsAst,
                                   llvm::Value* receiver) {
    // Method dispatch: built-ins parse argsAst positionally and can't
    // accept kwargs; UFCS and user methods route through the runtime
    // resolver via the dispatched closure's param metadata.
    bool has_kwargs = !arg_list_is_positional_only(argsAst);
    if (has_kwargs && known_builtin_methods().contains(method)) {
      emit_throw_error("SyntaxError",
          std::format("built-in method '{}' does not accept keyword "
                      "arguments", method),
          argsAst.line, argsAst.column);
    }
    // Set's mutating `.add(x)` / `.remove(x)` can shadow user-defined
    // Object methods of the same name (e.g. `Calculator.add(1)`). Emit
    // a runtime tag dispatch here so both worlds coexist: Set receiver
    // → set_add_method / set_remove; Object receiver → user property.
    if ((method == "add" || method == "remove") &&
        argsAst.nodes.size() == 1) {
      if (auto* r = compile_set_mutate_dispatch(method, argsAst, receiver)) {
        return r;
      }
    }
    if (auto* r =
            compile_builtin_method(method, argsAst, receiver)) {
      return r;
    }

    // UFCS fallback: if `method` is not a known builtin and resolves as
    // a free function in scope, `x.method(args)` becomes `method(x,
    // args)` — but only when the receiver has no user property by that
    // name. Existing methods always win (interpreter parity: Option 1
    // from the UFCS design discussion).
    bool is_builtin = known_builtin_methods().contains(method);
    const VarSlot* freeFnSlot = is_builtin ? nullptr : lookup_var(method);
    if (freeFnSlot) {
      return compile_method_or_ufcs(method, *freeFnSlot, argsAst, receiver);
    }

    // UFCS into an extension builtin: `x.puts()` → `puts(x)` and so on
    // for unary globals. These names live in `is_builtin_var` but don't
    // show up in `scopes_`, so the lookup_var branch above misses them.
    // Matches interp's UFCS resolution, which consults the full env.
    auto& h = current_hooks();
    if (h.compile_ufcs_builtin) {
      if (auto* r = h.compile_ufcs_builtin(*this, method, argsAst, receiver)) {
        return r;
      }
    }

    // Auto-synthesized `parameters()` on class instances. Mirrors the
    // interp branch in eval_property: when the receiver is a class
    // instance (Object with `class:` tag) and has no user-defined
    // `parameters` method, walk its fields and return a flat Array of
    // class-instance Values. User-defined parameters takes precedence.
    if (method == "parameters" && argsAst.nodes.empty()) {
      return compile_class_parameters_call(argsAst, receiver);
    }

    // User-defined method on Object: fetch property, call with this=receiver
    auto methodVal = compile_property_get(receiver, method);
    return compile_function_call(argsAst, methodVal, receiver);
  }

  // Auto-synthesized `parameters()` on class instances, JIT side. The
  // runtime-branch picks between (a) the synthesized walker when the
  // receiver is a class instance with no user-defined `parameters`,
  // and (b) the original property-get + call path for user overrides
  // and degenerate cases (non-Object receiver, plain dict). See
  // _jit_walk_collect_params for the walker semantics.
  llvm::Value* compile_class_parameters_call(const peg::Ast& argsAst,
                                             llvm::Value* receiver) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    auto tag = extract_tag(receiver);
    auto isObj = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT),
                                       "params.is.obj");

    auto checkBB = llvm::BasicBlock::Create(ctx_, "params.check", fn);
    auto autoBB = llvm::BasicBlock::Create(ctx_, "params.auto", fn);
    auto fallbackBB = llvm::BasicBlock::Create(ctx_, "params.fb", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "params.merge", fn);

    builder_.CreateCondBr(isObj, checkBB, fallbackBB);

    // checkBB: receiver is Object — auto-synth iff has `class:` tag and
    // no user-defined `parameters`.
    builder_.SetInsertPoint(checkBB);
    auto objPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto classKey = get_or_create_global_str("class", ".params.ck");
    auto paramsKey = get_or_create_global_str("parameters", ".params.pk");
    auto hasClass = emit_object_has(objPtr, classKey, "params.has.class");
    auto hasUser = emit_object_has(objPtr, paramsKey, "params.has.user");
    auto useAuto = builder_.CreateAnd(
        hasClass, builder_.CreateNot(hasUser), "params.use.auto");
    builder_.CreateCondBr(useAuto, autoBB, fallbackBB);

    // autoBB: call the runtime walker.
    builder_.SetInsertPoint(autoBB);
    auto walkResult = emit_call(
        module_->getOrInsertFunction(rt::class_parameters_walk, valueType_,
                                     ptrTy),
        {objPtr}, "params.walk");
    auto autoEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    // fallbackBB: original path. Errors out for non-Object receivers
    // (compile_property_get's type check), or invokes the user-defined
    // method, or fails with "expected Function, got Nil" for missing
    // method on a plain dict — all matching pre-auto-synth behavior.
    builder_.SetInsertPoint(fallbackBB);
    auto methodVal = compile_property_get(receiver, "parameters");
    auto fbResult = compile_function_call(argsAst, methodVal, receiver);
    auto fbEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 2, "params.result");
    phi->addIncoming(walkResult, autoEnd);
    phi->addIncoming(fbResult, fbEnd);
    return phi;
  }

  // `.add(x)` / `.remove(x)`: runtime tag dispatch.
  //   - TAG_SET    → set_add_method / set_remove (returns Bool)
  //   - TAG_OBJECT → for `remove`, the built-in object_remove_any
  //                  (returns Nil); for `add`, a user-defined property
  //                  with `this` bound to the receiver.
  //   - other      → type error
  // The argument is compiled once before the branch so side effects
  // don't duplicate.
  llvm::Value* compile_set_mutate_dispatch(const std::string& method,
                                           const peg::Ast& argsAst,
                                           llvm::Value* receiver) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();
    auto arg = compile(*argsAst.nodes[0]);

    auto tag = extract_tag(receiver);
    auto setBB = llvm::BasicBlock::Create(ctx_, "ma.set", fn);
    auto objBB = llvm::BasicBlock::Create(ctx_, "ma.obj", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "ma.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "ma.merge", fn);

    auto isSet =
        builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_SET));
    auto chkObjBB = llvm::BasicBlock::Create(ctx_, "ma.chk_obj", fn);
    builder_.CreateCondBr(isSet, setBB, chkObjBB);
    builder_.SetInsertPoint(chkObjBB);
    auto isObj =
        builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));
    builder_.CreateCondBr(isObj, objBB, errBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("Set or Object", tag);
    builder_.CreateUnreachable();

    // Set: mutating runtime helper.
    builder_.SetInsertPoint(setBB);
    auto setPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    const char* set_rt = method == "add" ? rt::set_add_method
                                         : rt::set_remove;
    auto setR = emit_call(
        module_->getOrInsertFunction(
            set_rt, builder_.getInt8Ty(), ptrTy,
            builder_.getInt8Ty(), builder_.getInt64Ty()),
        {setPtr, extract_tag(arg), extract_data(arg)});
    auto setRes = make_bool(builder_.CreateICmpNE(setR, builder_.getInt8(0)));
    // `set_remove` borrows the arg; `set_add_method` absorbs the +1.
    if (method == "remove") emit_value_release(arg);
    auto setEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    // Object: `remove` calls the built-in helper (returns Nil); `add`
    // routes to the user-defined property `this.add` (no built-in).
    builder_.SetInsertPoint(objBB);
    llvm::Value* objRes = nullptr;
    if (method == "remove") {
      auto objPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
      // object_remove_any consumes the key's +1 (see runtime helper).
      emit_call(
          module_->getOrInsertFunction(
              rt::object_remove_any, builder_.getVoidTy(), ptrTy,
              builder_.getInt8Ty(), builder_.getInt64Ty()),
          {objPtr, extract_tag(arg), extract_data(arg)});
      objRes = make_nil();
    } else {
      auto methodVal = compile_property_get(receiver, method);
      objRes = compile_function_call_raw(methodVal, receiver, {arg});
    }
    auto objEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 2, "ma.res");
    phi->addIncoming(setRes, setEnd);
    phi->addIncoming(objRes, objEnd);
    return phi;
  }

  // Emit the runtime dispatch between a user method (receiver owns a
  // property `method`) and UFCS (call the free variable `method` with
  // `receiver` as the first argument). Args are compiled once up front
  // so side-effects don't duplicate across branches.
  llvm::Value* compile_method_or_ufcs(const std::string& method,
                                      const VarSlot& freeFnSlot,
                                      const peg::Ast& argsAst,
                                      llvm::Value* receiver) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();

    // Kwargs path: runtime resolver per-branch — receiver flows into
    // `this_val` on the user-method branch and into `positional[0]` on
    // the UFCS branch, with the rest of the ARG_LIST compiled once and
    // routed through `culebra_runtime_call_with_kwargs`.
    if (!arg_list_is_positional_only(argsAst)) {
      auto methodHasBB =
          llvm::BasicBlock::Create(ctx_, "ufcs.kw.method", fn);
      auto ufcsHasBB =
          llvm::BasicBlock::Create(ctx_, "ufcs.kw.free", fn);
      auto mergeBB = llvm::BasicBlock::Create(ctx_, "ufcs.kw.merge", fn);

      auto rtag = extract_tag(receiver);
      auto isObj = builder_.CreateICmpEQ(rtag,
                                          builder_.getInt8(TAG_OBJECT));
      auto checkBB =
          llvm::BasicBlock::Create(ctx_, "ufcs.kw.check", fn);
      builder_.CreateCondBr(isObj, checkBB, ufcsHasBB);

      builder_.SetInsertPoint(checkBB);
      auto objPtr =
          builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
      auto keyPtr = get_or_create_global_str(method, ".mkey");
      auto hasProp = emit_object_has(objPtr, keyPtr);
      builder_.CreateCondBr(hasProp, methodHasBB, ufcsHasBB);

      builder_.SetInsertPoint(methodHasBB);
      // Receiver becomes `this_val`; retain so the +1 the helper
      // consumes is independent of the caller's reference.
      emit_value_retain(receiver);
      auto methodVal = compile_property_get(receiver, method);
      auto methodRes = compile_function_call_runtime_kwargs(
          argsAst, methodVal, receiver);
      auto methodEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(ufcsHasBB);
      // UFCS: receiver is positional[0]; retain separately.
      emit_value_retain(receiver);
      auto freeFn = load_slot(freeFnSlot, method);
      auto ufcsRes = compile_function_call_runtime_kwargs(
          argsAst, freeFn, /*thisVal=*/nullptr, {receiver});
      auto ufcsEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(mergeBB);
      auto phi = builder_.CreatePHI(valueType_, 2, "ufcs.kw.res");
      phi->addIncoming(methodRes, methodEndBB);
      phi->addIncoming(ufcsRes, ufcsEndBB);
      emit_value_release(receiver);
      return phi;
    }

    std::vector<llvm::Value*> userArgs;
    userArgs.reserve(argsAst.nodes.size());
    for (auto& argNode : argsAst.nodes) {
      userArgs.push_back(compile(*argNode));
    }

    auto tag = extract_tag(receiver);
    auto isObj = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));

    auto checkBB = llvm::BasicBlock::Create(ctx_, "ufcs.check", fn);
    auto methodBB = llvm::BasicBlock::Create(ctx_, "ufcs.method", fn);
    auto ufcsBB = llvm::BasicBlock::Create(ctx_, "ufcs.free", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "ufcs.merge", fn);

    // Non-Object receivers have no user properties, so UFCS always wins.
    builder_.CreateCondBr(isObj, checkBB, ufcsBB);

    builder_.SetInsertPoint(checkBB);
    auto objPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto keyPtr = get_or_create_global_str(method, ".mkey");
    auto hasProp = emit_object_has(objPtr, keyPtr);
    builder_.CreateCondBr(hasProp, methodBB, ufcsBB);

    builder_.SetInsertPoint(methodBB);
    auto methodVal = compile_property_get(receiver, method);
    auto methodRes =
        compile_function_call_raw(methodVal, receiver, userArgs);
    auto methodEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(ufcsBB);
    auto freeFn = load_slot(freeFnSlot, method);
    std::vector<llvm::Value*> ufcsArgs;
    ufcsArgs.reserve(userArgs.size() + 1);
    ufcsArgs.push_back(receiver);
    for (auto* a : userArgs) ufcsArgs.push_back(a);
    auto ufcsRes = compile_function_call_raw(freeFn, nullptr, ufcsArgs);
    auto ufcsEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 2, "ufcs.res");
    phi->addIncoming(methodRes, methodEndBB);
    phi->addIncoming(ufcsRes, ufcsEndBB);
    return phi;
  }

  // Raw call emission: caller has already compiled the user args.
  // Used by UFCS so a side-effectful arg expression is only lowered
  // once even though both branches need the resulting Values.
  //
  // ABI: Value fn(ptr cls, Value this, i64 n_args, ptr args). The
  // `args` slab is a stack alloca owned by this call; each entry
  // carries a +1 that the callee transfers into its param slot or
  // into `__ARGS__`.
  // `check_kw_only` triggers the runtime guard that throws when a
  // dynamic callee with a `*` separator receives more positionals
  // than allowed. Only the user-facing direct-call path enables it;
  // internal callers (static kwargs resolver, iter protocol, method
  // dispatch) drive pre-resolved slabs where the check would misfire.
  llvm::Value* compile_function_call_raw(
      llvm::Value* callee, llvm::Value* thisVal,
      llvm::ArrayRef<llvm::Value*> userArgs,
      bool check_kw_only = false) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    auto tag = extract_tag(callee);
    auto isFunc = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_FUNC));

    auto fn = builder_.GetInsertBlock()->getParent();
    auto callBB = llvm::BasicBlock::Create(ctx_, "call.ok", fn);
    auto errorBB = llvm::BasicBlock::Create(ctx_, "call.error", fn);
    builder_.CreateCondBr(isFunc, callBB, errorBB);

    builder_.SetInsertPoint(errorBB);
    emit_type_error_typed("Function", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(callBB);
    auto clsPtr = builder_.CreateIntToPtr(extract_data(callee), ptrTy);
    auto fnFieldPtr =
        builder_.CreateStructGEP(closureType_, clsPtr, 1, "fn.ptr");
    auto fnPtr = builder_.CreateLoad(ptrTy, fnFieldPtr, "fn");

    if (check_kw_only) {
      emit_call(
          module_->getFunction(rt::check_pos_count),
          {fnPtr,
           builder_.getInt64(static_cast<int64_t>(userArgs.size())),
           current_line_val(), current_column_val()});
    }

    // Build the args slab in the entry block (hoisted) so repeated
    // calls within a loop don't grow the stack frame per iteration.
    llvm::Value* argsPtr;
    if (userArgs.empty()) {
      argsPtr = llvm::ConstantPointerNull::get(ptrTy);
    } else {
      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      argsPtr = entryB.CreateAlloca(
          valueType_,
          builder_.getInt64(static_cast<int64_t>(userArgs.size())),
          "call.args");
      for (size_t i = 0; i < userArgs.size(); i++) {
        auto slot = builder_.CreateInBoundsGEP(
            valueType_, argsPtr,
            {builder_.getInt64(static_cast<int64_t>(i))});
        builder_.CreateStore(userArgs[i], slot);
      }
    }

    auto calleeType = llvm::FunctionType::get(
        valueType_, {ptrTy, valueType_, builder_.getInt64Ty(), ptrTy},
        false);
    std::vector<llvm::Value*> args = {
        clsPtr,
        thisVal ? thisVal : make_nil(),
        builder_.getInt64(static_cast<int64_t>(userArgs.size())),
        argsPtr};
    return emit_call(calleeType, fnPtr, args, "call.result");
  }

  // True if every ARG_LIST child is a plain positional expression
  // (no `name:` kwarg and no `**splat`). Lets the JIT take its fast
  // path; the kwargs path is only entered when this is false.
  static bool arg_list_is_positional_only(const peg::Ast& argsAst) {
    using namespace peg::udl;
    for (auto& child : argsAst.nodes) {
      if (child->tag == "KWARG"_ || child->tag == "KWARG_SPLAT"_) {
        return false;
      }
    }
    return true;
  }

  // Walk scopes inner-to-outer for a `let f = fn (...) {...}` style
  // binding the JIT can statically resolve kwargs against. Returns
  // null for captured / reassigned / built-in callees.
  const peg::Ast* lookup_fn_ast(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->fn_asts.find(name);
      if (found != it->fn_asts.end()) return found->second;
    }
    return nullptr;
  }

  // Compile-time resolution of kwargs/splats for a directly-named user
  // function whose FUNCTION AST we have. Reorders the ARG_LIST into
  // positional form matching the formal parameters, then calls through
  // the regular slab ABI. Trailing-defaulted-only — a middle gap (an
  // unfilled defaulted slot followed by a filled one) is an ArityError.
  llvm::Value* compile_function_call_with_kwargs(
      const peg::Ast& argsAst, llvm::Value* callee,
      const peg::Ast& fnAst, llvm::Value* thisVal = nullptr) {
    using namespace peg::udl;

    // FUNCTION layout: [PARAMETERS, (RETURN_TYPE)?, BLOCK]. Each
    // PARAMETER child: [MUTABLE, IDENTIFIER, (TYPE_ANNOTATION)?,
    // (DEFAULT_VALUE)?].
    const auto& paramsAst = *fnAst.nodes[0];
    struct ParamInfo {
      std::string_view name;
      bool has_default;
      bool kw_only;
      bool kwargs_rest;
    };
    std::vector<ParamInfo> params;
    params.reserve(paramsAst.nodes.size());
    bool kw_only = false;
    std::optional<size_t> rest_idx;
    for (auto& p : paramsAst.nodes) {
      auto pv = culebra::view_parameter(*p);
      if (pv.is_kw_only_sep) {
        kw_only = true;
        continue;
      }
      if (pv.is_kwargs_rest) {
        params.push_back({pv.name, false, false, true});
        rest_idx = params.size() - 1;
        continue;
      }
      params.push_back({
          pv.name,
          pv.default_value != nullptr,
          kw_only,
          false,
      });
    }

    // Scan ARG_LIST → positional + kwargs.
    //
    // Two passes so the semantics match the interp resolver
    // (`bind_call_args` in interpreter.h): all splats merge first
    // (later splat overrides earlier), then explicit kwargs layer on
    // top regardless of source order. So `f(c: 3, **{c: 5})` always
    // resolves to `c = 3` because the explicit wins, just like
    // `f(**{c: 5}, c: 3)`.
    std::vector<const peg::Ast*> positional;
    std::map<std::string_view, const peg::Ast*> kwargs;
    std::vector<std::pair<std::string_view, const peg::Ast*>>
        explicit_kwargs;
    std::set<std::string_view> seen_explicit;
    bool saw_named = false;
    for (auto& child : argsAst.nodes) {
      if (child->tag == "KWARG_SPLAT"_) {
        saw_named = true;
        // Caller (the CALL postfix dispatcher) only routes here when
        // every splat is a literal OBJECT — dynamic splats fall back
        // to `compile_function_call_runtime_kwargs`. So we can read
        // the OBJECT literal's properties straight from the AST.
        const auto& operand = *child->nodes[0];
        for (auto& prop : operand.nodes) {
          const auto& key_node = *prop->nodes[1];
          if (key_node.tag != "IDENTIFIER"_) {
            // Emit a runtime throw and skip this entry — inserting a
            // non-identifier token into the kwargs map would corrupt
            // the subsequent resolver state for downstream IR even
            // though the throw fires first at runtime.
            emit_throw_error("TypeError",
                "**: splat Object key must be an identifier",
                argsAst.line, argsAst.column);
            continue;
          }
          const peg::Ast* val_ast = prop->nodes.size() >= 3
              ? prop->nodes[2].get()
              : &key_node;
          kwargs[key_node.token] = val_ast;
        }
      } else if (child->tag == "KWARG"_) {
        saw_named = true;
        auto name = child->nodes[0]->token;
        if (!seen_explicit.insert(name).second) {
          emit_throw_error("TypeError",
              std::format("duplicate keyword argument '{}'", name),
              argsAst.line, argsAst.column);
        }
        explicit_kwargs.emplace_back(name, child->nodes[1].get());
      } else {
        if (saw_named) {
          emit_throw_error("SyntaxError",
              "positional argument follows keyword argument",
              argsAst.line, argsAst.column);
        }
        positional.push_back(child.get());
      }
    }
    // Layer explicit kwargs on top of the splat-merged map.
    for (const auto& [name, val] : explicit_kwargs) {
      kwargs[name] = val;
    }

    // Resolve to positional order. Each slot gets filled from either
    // positional or kwargs; if neither, the slot must have a default
    // and is marked with TAG_UNFILLED so the callee prologue falls
    // back to its inline default expression. Middle gaps work the
    // same way as trailing gaps — both use the sentinel.
    enum class Source { None, Positional, Kwarg, KwargsRest };
    std::vector<Source> sources(params.size(), Source::None);
    std::vector<const peg::Ast*> resolved(params.size(), nullptr);
    {
      auto cap = culebra::first_kw_only_index(params);
      long cap_long = cap ? static_cast<long>(*cap) : -1;
      long n_pos = static_cast<long>(positional.size());
      // Match throw_if_too_many_positionals' compile-time predicate
      // but route through emit_throw_error so the runtime throw is
      // catchable by `try { ... } catch e { ... }`, the same way
      // interp's eval-time call at interpreter.h:4220 is.
      if (cap_long > 0 && n_pos > cap_long) {
        emit_throw_error("TypeError",
            std::format("takes {} positional argument{} but {} given",
                        cap_long, cap_long == 1 ? "" : "s", n_pos),
            argsAst.line, argsAst.column);
      }
    }
    for (size_t i = 0; i < params.size(); i++) {
      if (params[i].kwargs_rest) continue;
      if (i < positional.size() && !params[i].kw_only) {
        if (kwargs.contains(params[i].name)) {
          emit_throw_error("TypeError",
              std::format("got argument '{}' both positionally and as "
                          "a keyword", params[i].name),
              argsAst.line, argsAst.column);
        }
        resolved[i] = positional[i];
        sources[i] = Source::Positional;
      } else if (auto it = kwargs.find(params[i].name); it != kwargs.end()) {
        resolved[i] = it->second;
        sources[i] = Source::Kwarg;
        kwargs.erase(it);
      } else if (!params[i].has_default) {
        // Defer to runtime so `try { f(...) } catch e { ... }` can
        // observe the same ArityError interp produces. The IR after
        // the throw is dead at runtime; we leave the slot as
        // Source::None so the surrounding slab-fill logic emits a
        // syntactically valid TAG_UNFILLED placeholder.
        emit_missing_required_arg_throw(std::string(params[i].name),
                                         argsAst.line, argsAst.column);
      }
    }
    if (rest_idx) {
      sources[*rest_idx] = Source::KwargsRest;
      // The rest slot is always emitted (Object), even when empty.
    } else if (!kwargs.empty()) {
      // Same rationale as the missing-required throw above — emit at
      // runtime so the error is catchable. Clear the map so subsequent
      // resolution paths see a consistent empty state.
      auto bad_name = std::string(kwargs.begin()->first);
      emit_unknown_kwarg_throw(bad_name, argsAst.line, argsAst.column);
      kwargs.clear();
    }

    std::vector<llvm::Value*> userArgs;
    userArgs.reserve(params.size() +
                     (positional.size() > params.size()
                          ? positional.size() - params.size()
                          : 0));
    // Trim trailing TAG_UNFILLED slots — the callee prologue's
    // existing `n_args > i` check handles them via the default path
    // without needing the sentinel.
    size_t fill_end = params.size();
    while (fill_end > 0 && sources[fill_end - 1] == Source::None) {
      fill_end--;
    }
    for (size_t i = 0; i < fill_end; i++) {
      if (sources[i] == Source::None) {
        // Middle gap with a default — emit TAG_UNFILLED sentinel.
        llvm::Value* v = llvm::UndefValue::get(valueType_);
        v = builder_.CreateInsertValue(
            v, builder_.getInt8(TAG_UNFILLED), {0});
        v = builder_.CreateInsertValue(
            v, builder_.getInt64(0), {1});
        userArgs.push_back(v);
      } else if (sources[i] == Source::KwargsRest) {
        userArgs.push_back(emit_kwargs_rest_object(kwargs, argsAst));
      } else {
        userArgs.push_back(compile(*resolved[i]));
      }
    }
    // Extras (past the formal arity) flow through to __ARGS__.
    for (size_t i = params.size(); i < positional.size(); i++) {
      userArgs.push_back(compile(*positional[i]));
    }
    return compile_function_call_raw(callee, thisVal, userArgs);
  }

  llvm::Value* compile_function_call(const peg::Ast& argsAst,
                                     llvm::Value* callee,
                                     llvm::Value* thisVal = nullptr) {
    if (!arg_list_is_positional_only(argsAst)) {
      return compile_function_call_runtime_kwargs(
          argsAst, callee, thisVal);
    }
    std::vector<llvm::Value*> userArgs;
    userArgs.reserve(argsAst.nodes.size());
    for (auto& argNode : argsAst.nodes) {
      userArgs.push_back(compile(*argNode));
    }
    return compile_function_call_raw(callee, thisVal, userArgs,
                                     /*check_kw_only=*/any_kw_only_in_program_);
  }

  // Indirect / UFCS / method kwargs path. Compiles ARG_LIST into
  // separate positional / kwarg-key / kwarg-val / splat slabs and
  // hands them to `culebra_runtime_call_with_kwargs`, which consults
  // the closure's registered param metadata to resolve names. Splat
  // operands can be runtime Objects (the helper enumerates them).
  //
  // `positional_prefix` lets UFCS prepend the receiver as positional
  // arg #0 — the prefix values are already-compiled +1 Values; they
  // join the ARG_LIST's positional entries (which are compiled here)
  // ahead of any kwarg or splat.
  llvm::Value* compile_function_call_runtime_kwargs(
      const peg::Ast& argsAst, llvm::Value* callee,
      llvm::Value* thisVal,
      const std::vector<llvm::Value*>& positional_prefix = {}) {
    using namespace peg::udl;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i64Ty = builder_.getInt64Ty();

    // Scan ARG_LIST. Two-pass merge happens inside the runtime helper
    // so the IR scan only enforces structural rules.
    std::vector<const peg::Ast*> positional;
    std::vector<std::pair<std::string_view, const peg::Ast*>>
        explicit_kwargs;
    std::set<std::string_view> seen_explicit;
    std::vector<const peg::Ast*> splats;
    bool saw_named = false;
    for (auto& child : argsAst.nodes) {
      if (child->tag == "KWARG_SPLAT"_) {
        saw_named = true;
        splats.push_back(child->nodes[0].get());
      } else if (child->tag == "KWARG"_) {
        saw_named = true;
        auto name = child->nodes[0]->token;
        if (!seen_explicit.insert(name).second) {
          emit_throw_error("TypeError",
              std::format("duplicate keyword argument '{}'", name),
              argsAst.line, argsAst.column);
        }
        explicit_kwargs.emplace_back(name, child->nodes[1].get());
      } else {
        if (saw_named) {
          emit_throw_error("SyntaxError",
              "positional argument follows keyword argument",
              argsAst.line, argsAst.column);
        }
        positional.push_back(child.get());
      }
    }

    // Pre-size the per-bucket value vectors and pre-build kwarg key
    // global strings (these have no side effects). The actual value
    // expressions are compiled in a single source-order pass below
    // so the interp's left-to-right evaluation order is preserved
    // even when positional / kwarg / **splat are interleaved.
    std::vector<llvm::Value*> posVals(
        positional_prefix.size() + positional.size(), nullptr);
    for (size_t i = 0; i < positional_prefix.size(); i++) {
      posVals[i] = positional_prefix[i];
    }
    std::vector<llvm::Value*> kwVals(explicit_kwargs.size(), nullptr);
    std::vector<llvm::Constant*> kwKeyConsts;
    kwKeyConsts.reserve(explicit_kwargs.size());
    for (auto& [name, _ast] : explicit_kwargs) {
      kwKeyConsts.push_back(builder_.CreateGlobalString(
          std::string(name), ".kwkey"));
    }
    std::vector<llvm::Value*> splatVals(splats.size(), nullptr);

    // Source-order evaluation: walk the original ARG_LIST and compile
    // each value where it appears, storing into the right bucket
    // index. Matches interp's `split_call_args` + sequential eval.
    size_t pos_i = positional_prefix.size();
    size_t kw_i = 0;
    size_t splat_i = 0;
    for (auto& child : argsAst.nodes) {
      if (child->tag == "KWARG_SPLAT"_) {
        splatVals[splat_i++] = compile(*child->nodes[0]);
      } else if (child->tag == "KWARG"_) {
        kwVals[kw_i++] = compile(*child->nodes[1]);
      } else {
        posVals[pos_i++] = compile(*child);
      }
    }

    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                              fn->getEntryBlock().begin());

    auto alloc_slab = [&](llvm::Type* ty, size_t n, const char* name)
        -> llvm::Value* {
      if (n == 0) return llvm::ConstantPointerNull::get(ptrTy);
      return entryB.CreateAlloca(
          ty, builder_.getInt64(static_cast<int64_t>(n)), name);
    };
    auto posSlab = alloc_slab(valueType_, posVals.size(), "kwc.pos");
    auto kwKeysSlab = alloc_slab(ptrTy, kwKeyConsts.size(), "kwc.keys");
    auto kwValsSlab = alloc_slab(valueType_, kwVals.size(), "kwc.vals");
    auto splatSlab = alloc_slab(valueType_, splatVals.size(), "kwc.splat");

    auto store_at = [&](llvm::Value* base, llvm::Type* ty, size_t i,
                         llvm::Value* v) {
      auto slot = builder_.CreateInBoundsGEP(
          ty, base, {builder_.getInt64(static_cast<int64_t>(i))});
      builder_.CreateStore(v, slot);
    };
    for (size_t i = 0; i < posVals.size(); i++) {
      store_at(posSlab, valueType_, i, posVals[i]);
    }
    for (size_t i = 0; i < kwKeyConsts.size(); i++) {
      store_at(kwKeysSlab, ptrTy, i, kwKeyConsts[i]);
      store_at(kwValsSlab, valueType_, i, kwVals[i]);
    }
    for (size_t i = 0; i < splatVals.size(); i++) {
      store_at(splatSlab, valueType_, i, splatVals[i]);
    }

    // Callee must be a closure; type-check at the IR level (matches
    // `compile_function_call_raw`'s guard).
    auto tag = extract_tag(callee);
    auto isFunc = builder_.CreateICmpEQ(tag,
                                         builder_.getInt8(TAG_FUNC));
    auto okBB = llvm::BasicBlock::Create(ctx_, "kwc.callee.ok", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "kwc.callee.err", fn);
    builder_.CreateCondBr(isFunc, okBB, errBB);
    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("Function", tag);
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(okBB);
    auto clsPtr = builder_.CreateIntToPtr(extract_data(callee), ptrTy);

    auto thisVV = thisVal ? thisVal : make_nil();

    auto result = emit_call(
        module_->getOrInsertFunction(
            rt::call_with_kwargs, valueType_, ptrTy, valueType_,
            i64Ty, ptrTy, i64Ty, ptrTy, ptrTy, i64Ty, ptrTy,
            i64Ty, i64Ty),
        {clsPtr, thisVV,
         builder_.getInt64(static_cast<int64_t>(posVals.size())), posSlab,
         builder_.getInt64(static_cast<int64_t>(kwVals.size())),
         kwKeysSlab, kwValsSlab,
         builder_.getInt64(static_cast<int64_t>(splatVals.size())),
         splatSlab, current_line_val(), current_column_val()});
    // The callee ref follows `compile_function_call_raw`'s convention:
    // it is borrowed for the duration of the dispatch, not released
    // here. Outer compile_call leaks one ref per call as a pre-existing
    // limitation independent of kwargs.
    return result;
  }

  // Lower `range(N)` / `range(start, end)` / `iota(N)` / `iota(start,
  // end)` to a single runtime call. `range` additionally accepts a
  // `step:` kw-only argument. Returns nullptr if the callee is not
  // one of these or the shape is wrong, letting the regular dispatch
  // take over.
  llvm::Value* try_compile_core_global(const std::string& name,
                                        const peg::Ast& argsAst) {
    using namespace peg::udl;
    // Collect positional args + any `step:` kwarg (range-only). Any
    // splat or unknown kwarg bails to the regular dispatch.
    std::vector<const peg::Ast*> positional;
    const peg::Ast* step_ast = nullptr;
    for (auto& c : argsAst.nodes) {
      if (c->tag == "KWARG_SPLAT"_) return nullptr;
      if (c->tag == "KWARG"_) {
        if (name != "range" || c->nodes[0]->token != "step") return nullptr;
        step_ast = c->nodes[1].get();
      } else {
        positional.push_back(c.get());
      }
    }
    auto two_args = [&](llvm::Value*& s, llvm::Value*& e) {
      if (positional.size() == 1) {
        s = builder_.getInt64(0);
        e = value_to_long(compile(*positional[0]));
        return true;
      }
      if (positional.size() == 2) {
        s = value_to_long(compile(*positional[0]));
        e = value_to_long(compile(*positional[1]));
        return true;
      }
      return false;
    };
    llvm::Value *s = nullptr, *e = nullptr;
    if (name == "iota" && !step_ast && two_args(s, e)) {
      auto arr = builder_.CreateCall(module_->getFunction(rt::iota), {s, e});
      return make_array(arr);
    }
    if (name == "range" && two_args(s, e)) {
      auto step = step_ast ? value_to_long(compile(*step_ast))
                           : builder_.getInt64(1);
      auto obj = emit_call(
          module_->getFunction(rt::math_range),
          {s, e, step, current_line_val(), current_column_val()});
      return make_object(obj);
    }
    return nullptr;
  }

  // Fuse `range(N).<HOF>(...)` into a direct counter loop, skipping
  // the iterator wrapper and per-element trampoline. Recognized HOFs:
  // .reduce(init, |acc,i|), .for_each(|i|), .map(|i|).collect().
  // ~17% of HOF microgpt's runtime is in these shapes; bypassing the
  // wrapper keeps the loop alloc-free.
  llvm::Value* try_compile_range_fusion(const peg::Ast& ast) {
    using namespace peg::udl;
    if (ast.nodes.empty()) return nullptr;
    const auto& calleeNode = *ast.nodes[0];
    if (calleeNode.tag != "IDENTIFIER"_) return nullptr;
    if (calleeNode.token != "range") return nullptr;
    // Bail out on any kwarg/splat in either ARGUMENTS so the normal
    // dispatch's guard surfaces a clean error.
    for (size_t i = 1; i < ast.nodes.size(); i++) {
      if (ast.nodes[i]->original_tag == "ARGUMENTS"_ &&
          !arg_list_is_positional_only(*ast.nodes[i])) {
        return nullptr;
      }
    }
    if (ast.nodes.size() < 4 ||
        ast.nodes[1]->original_tag != "ARGUMENTS"_ ||
        ast.nodes[1]->nodes.size() != 1 ||
        ast.nodes[2]->original_tag != "DOT"_ ||
        ast.nodes[3]->original_tag != "ARGUMENTS"_) {
      return nullptr;
    }
    const auto& n_ast = *ast.nodes[1]->nodes[0];
    auto method = ast.nodes[2]->token;
    const auto& m_args = *ast.nodes[3];
    if (method == "reduce" && m_args.nodes.size() == 2 &&
        is_inlinable_lambda(*m_args.nodes[1], 2) &&
        ast.nodes.size() == 4) {
      return emit_inlined_range_reduce(
          n_ast, *m_args.nodes[0], *m_args.nodes[1]);
    }
    if (method == "for_each" && m_args.nodes.size() == 1 &&
        is_inlinable_lambda(*m_args.nodes[0], 1) &&
        ast.nodes.size() == 4) {
      return emit_inlined_range_for_each(n_ast, *m_args.nodes[0]);
    }
    if (method == "map" && m_args.nodes.size() == 1 &&
        is_inlinable_lambda(*m_args.nodes[0], 1) &&
        ast.nodes.size() == 6 &&
        ast.nodes[4]->original_tag == "DOT"_ &&
        ast.nodes[4]->token == "collect" &&
        ast.nodes[5]->original_tag == "ARGUMENTS"_ &&
        ast.nodes[5]->nodes.empty()) {
      return emit_inlined_range_map_collect(n_ast, *m_args.nodes[0]);
    }
    return nullptr;
  }

  // Handle a CALL. The core lowers `range`/`iota` (and the
  // `range(N).<HOF>(...)` fusion) directly; everything else delegates
  // to the registered extension via the install_extension hooks. With
  // no extension installed every hook is a no-op and this falls
  // through to the regular CALL dispatch.
  llvm::Value* compile_call_with_builtins(const peg::Ast& ast) {
    using namespace peg::udl;
    auto calleeNode = ast.nodes[0];

    if (auto v = try_compile_range_fusion(ast)) return v;

    llvm::Value* start = nullptr;
    size_t next_idx = 1;

    auto& h = current_hooks();
    if (calleeNode->tag == "IDENTIFIER"_ && ast.nodes.size() >= 2 &&
        ast.nodes[1]->original_tag == "ARGUMENTS"_) {
      auto name = std::string(calleeNode->token);
      start = try_compile_core_global(name, *ast.nodes[1]);
      if (!start && h.compile_global) {
        start = h.compile_global(*this, name, *ast.nodes[1], ast);
      }
      if (start) next_idx = 2;
    }

    if (!start && calleeNode->tag == "IDENTIFIER"_ &&
        ast.nodes.size() >= 2 &&
        ast.nodes[1]->original_tag == "DOT"_) {
      auto ns = calleeNode->token;
      auto prop = ast.nodes[1]->token;
      if (ast.nodes.size() >= 3 &&
          ast.nodes[2]->original_tag == "ARGUMENTS"_ &&
          h.compile_ns_call) {
        start = h.compile_ns_call(*this, ns, prop, *ast.nodes[2], ast);
        if (start) next_idx = 3;
      }
      if (!start && h.compile_ns_prop) {
        start = h.compile_ns_prop(*this, ns, prop);
        if (start) next_idx = 2;
      }
    }

    if (!start) return compile_call(ast);

    // Continue with remaining postfixes (matching compile_call's loop).
    llvm::Value* callee = start;
    for (auto i = next_idx; i < ast.nodes.size(); i++) {
      const auto& postfix = *ast.nodes[i];
      switch (postfix.original_tag) {
        case "ARGUMENTS"_:
          callee = compile_function_call(postfix, callee);
          break;
        case "INDEX"_: {
          auto receiver = callee;
          callee = compile_index_access(postfix, receiver);
          emit_value_swap_owned(callee, receiver);
          break;
        }
        case "DOT"_: {
          if (i + 1 < ast.nodes.size() &&
              ast.nodes[i + 1]->original_tag == "ARGUMENTS"_) {
            auto method = std::string(postfix.token);
            if (method == "map") {
              if (auto fused = try_fuse_iter_map_collect(ast, i, callee)) {
                callee = *fused;
                i += 3;  // consume ARGUMENTS, DOT(collect), ARGUMENTS
                break;
              }
            }
            callee = compile_method_call(method, *ast.nodes[i + 1], callee);
            i++;
          } else {
            auto name = std::string(postfix.token);
            auto receiver = callee;
            callee = compile_property_get(receiver, name);
            // Function introspection (.name / .params / .return_type)
            // returns a fresh +1 owned value rather than a borrowed
            // Object IC slot view, so swap_owned would double-retain it
            // and leak. Release the receiver directly instead.
            if (name == "name" || name == "params" || name == "return_type") {
              emit_value_release(receiver);
            } else {
              emit_value_swap_owned(callee, receiver);
            }
          }
          break;
        }
        default:
          throw std::runtime_error("invalid call postfix");
      }
    }
    return callee;
  }

  // --- Debugger ---

  llvm::Value* compile_debugger(const peg::Ast& ast) {
    if (!debug_enabled_) return make_nil();
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto pathPtr = builder_.CreateGlobalString(ast.path, ".dbgpath");
    emit_call(
        module_->getOrInsertFunction(
            rt::debugger_break, builder_.getVoidTy(), ptrTy,
            builder_.getInt64Ty(), builder_.getInt64Ty()),
        {pathPtr, builder_.getInt64(static_cast<int64_t>(ast.line)),
         builder_.getInt64(static_cast<int64_t>(ast.column))});
    return make_nil();
  }

  // --- Return ---

  llvm::Value* compile_return(const peg::Ast& ast) {
    llvm::Value* val;
    if (ast.nodes.empty()) {
      val = make_nil();
    } else {
      val = compile(*ast.nodes[0]);
    }
    if (!current_return_type_.empty()) {
      emit_type_check(val, current_return_type_, "return value");
    }
    // Run any defers registered in this function's scopes before
    // returning to the caller. No invoke needed: we're exiting.
    if (current_fn_defer_mark_) {
      builder_.CreateCall(
          module_->getFunction(rt::defer_run_to),
          {current_fn_defer_mark_});
    }
    release_all_scopes_for_exit();
    builder_.CreateRet(val);
    return val;
  }

  // `defer { BODY }` — compiles BODY as a 0-param closure and pushes
  // it onto the global defer stack. The closure captures any outer
  // variables it references, via the same cell mechanism as regular
  // fn literals. The surrounding scope's exit path (fall-through,
  // return, throw) calls `culebra_runtime_defer_run_to` to unwind the
  // stack back to that scope's mark.
  llvm::Value* compile_defer(const peg::Ast& ast) {
    using namespace llvm;
    auto ptrTy = PointerType::get(ctx_, 0);

    auto infoIt = func_info_.find(&ast);
    if (infoIt == func_info_.end()) {
      throw std::runtime_error("missing func_info for defer");
    }
    FuncInfo& info = infoIt->second;

    // Snapshot outer mut flags before compiling the defer body — the
    // body's free-var bindings read this. Mirrors compile_fn_common.
    info.free_var_mut.assign(info.free_vars.size(), false);
    for (size_t i = 0; i < info.free_vars.size(); i++) {
      auto outer_slot = lookup_var(info.free_vars[i]);
      if (outer_slot) info.free_var_mut[i] = outer_slot->mut;
    }

    // Same uniform ABI as compile_function; defer thunks ignore
    // n_args/args (callers always pass 0/null).
    auto fnType = FunctionType::get(
        valueType_, {ptrTy, valueType_, builder_.getInt64Ty(), ptrTy},
        false);
    auto fnName = std::format("__culebra_defer_{}", funcCounter_++);
    auto fn = Function::Create(fnType, GlobalValue::ExternalLinkage, fnName,
                               module_);
    if (info.has_eh) {
      fn->setPersonalityFn(get_personality_fn());
    }

    auto argIt = fn->arg_begin();
    argIt->setName("__cls__");
    ++argIt;
    argIt->setName("this");
    ++argIt;
    argIt->setName("n_args");
    ++argIt;
    argIt->setName("args");

    CompilerStateSaver saver(*this);
    current_info_ = &info;

    auto entryBB = BasicBlock::Create(ctx_, "entry", fn);
    builder_.SetInsertPoint(entryBB);
    push_scope();

    argIt = fn->arg_begin();
    auto clsArg = &*argIt++;
    current_closure_arg_ = clsArg;

    // Bind free variables: load cell pointers from closure->captures[i].
    for (size_t i = 0; i < info.free_vars.size(); i++) {
      const auto& fv = info.free_vars[i];
      auto capturesFieldPtr =
          builder_.CreateStructGEP(closureType_, clsArg, 3, "caps.ptr");
      auto capturesArr =
          builder_.CreateLoad(ptrTy, capturesFieldPtr, "caps");
      auto cellSlotPtr = builder_.CreateInBoundsGEP(
          ptrTy, capturesArr, {builder_.getInt64(i)}, "cell.slot");
      auto cellPtr = builder_.CreateLoad(ptrTy, cellSlotPtr, fv + ".cell");
      llvm::IRBuilder<> entryB(entryBB, entryBB->begin());
      auto holder = entryB.CreateAlloca(ptrTy, nullptr, fv);
      builder_.CreateStore(cellPtr, holder);
      // Borrowed: the defer closure object owns the cell ref.
      bool fv_mut = i < info.free_var_mut.size() ? info.free_var_mut[i] : false;
      define_var(fv, VarSlot{VarSlot::Cell, holder, /*owned=*/false, fv_mut});
    }

    compile(*ast.nodes[0]);
    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateRet(make_nil());
    }

    pop_scope();
    verifyFunction(*fn);

    saver.restore();

    // Build the 0-arg closure and push it onto the defer stack. The
    // runtime retains it; we drop the local ref after.
    auto closureVal = emit_closure_build(fn, info, 0);
    builder_.CreateCall(
        module_->getFunction(rt::defer_push),
        {extract_tag(closureVal), extract_data(closureVal)});
    emit_value_release(closureVal);
    return make_nil();
  }

  // `throw expr` — evaluates expr, hands its tag/data to the runtime
  // which raises a CulebraException. Control never falls through.
  llvm::Value* compile_throw(const peg::Ast& ast) {
    auto val = compile(*ast.nodes[0]);
    emit_call(module_->getFunction(rt::throw_),
              {extract_tag(val), extract_data(val)});
    builder_.CreateUnreachable();
    // Fresh dead block so any trailing code has a valid insert point.
    auto deadBB = llvm::BasicBlock::Create(
        ctx_, "throw.dead", builder_.GetInsertBlock()->getParent());
    builder_.SetInsertPoint(deadBB);
    return make_nil();
  }

  // `try BODY catch name HANDLER` — evaluates BODY. If BODY throws a
  // CulebraException, HANDLER runs with `name` bound to the caught
  // value. The whole form is an expression yielding whichever block
  // ran last. `return` inside BODY / HANDLER returns from the
  // enclosing user function (standard LLVM ret semantics).
  llvm::Value* compile_try(const peg::Ast& ast) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();

    auto tryBB = llvm::BasicBlock::Create(ctx_, "try.body", fn);
    auto lpadBB = llvm::BasicBlock::Create(ctx_, "try.lpad", fn);
    auto catchBB = llvm::BasicBlock::Create(ctx_, "try.catch", fn);
    auto endBB = llvm::BasicBlock::Create(ctx_, "try.end", fn);

    llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    auto resultSlot = entryB.CreateAlloca(valueType_, nullptr, "try.result");
    builder_.CreateStore(make_nil(), resultSlot);
    auto caughtSlot = entryB.CreateAlloca(valueType_, nullptr, "try.caught");

    builder_.CreateBr(tryBB);

    // --- Try body: compile with current_lpad_ set to lpadBB ---
    builder_.SetInsertPoint(tryBB);
    auto savedLpad = current_lpad_;
    current_lpad_ = lpadBB;
    push_scope();
    auto tryVal = compile(*ast.nodes[0]);
    pop_scope();
    current_lpad_ = savedLpad;
    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateStore(tryVal, resultSlot);
      builder_.CreateBr(endBB);
    }

    // --- Landing pad: catch-all. begin_catch enters the C++ catch
    // context, then `culebra_runtime_try_translate` inspects the
    // in-flight exception. If it's a user throw (`culebra_is_throw==1`
    // already, set by `culebra_runtime_throw`) or a CulebraError /
    // std::runtime_error from a runtime helper (set by translate),
    // read the carried tag/data and proceed to the catch body.
    // Otherwise rethrow so the foreign exception keeps unwinding. ---
    builder_.SetInsertPoint(lpadBB);
    auto lpadTy = llvm::StructType::get(ptrTy, builder_.getInt32Ty());
    auto lpad = builder_.CreateLandingPad(lpadTy, 1, "exc");
    lpad->addClause(llvm::ConstantPointerNull::get(ptrTy));  // catch-all

    // Enter the C++ catch context. begin_catch must run before
    // translate so its `try { throw; }` re-raises the active exception.
    auto excPtr = builder_.CreateExtractValue(lpad, {0}, "exc.ptr");
    auto beginCatch = module_->getOrInsertFunction(
        "__cxa_begin_catch", ptrTy, ptrTy);
    builder_.CreateCall(beginCatch, {excPtr});
    auto translateFn = module_->getOrInsertFunction(
        "culebra_runtime_try_translate", builder_.getVoidTy());
    builder_.CreateCall(translateFn);

    // Load the flag: `culebra_runtime_throw` set it for user throws,
    // `culebra_runtime_try_translate` for catchable C++ runtime errors.
    // Either way, 1 means we have a value to bind to the catch name.
    auto getIsThrow = module_->getOrInsertFunction(
        "culebra_runtime_get_is_throw", builder_.getInt8Ty());
    auto flagVal = builder_.CreateCall(getIsThrow, {}, "is_throw");
    auto isOurs = builder_.CreateICmpNE(flagVal, builder_.getInt8(0));
    auto handleBB = llvm::BasicBlock::Create(ctx_, "try.handle", fn);
    auto notOursBB = llvm::BasicBlock::Create(ctx_, "try.notours", fn);
    builder_.CreateCondBr(isOurs, handleBB, notOursBB);

    // Not classifiable — rethrow so the foreign exception keeps
    // unwinding past this try/catch.
    builder_.SetInsertPoint(notOursBB);
    emit_rethrow(savedLpad);

    builder_.SetInsertPoint(handleBB);
    // Clear the flag and consume the exception.
    auto clearIsThrow = module_->getOrInsertFunction(
        "culebra_runtime_clear_is_throw", builder_.getVoidTy());
    builder_.CreateCall(clearIsThrow);
    auto endCatchFn = module_->getOrInsertFunction("__cxa_end_catch",
                                                   builder_.getVoidTy());
    builder_.CreateCall(endCatchFn);
    // Read the thrown Value via accessors (see thread-local note above).
    auto getTag = module_->getOrInsertFunction(
        "culebra_runtime_get_thrown_tag", builder_.getInt8Ty());
    auto getData = module_->getOrInsertFunction(
        "culebra_runtime_get_thrown_data", builder_.getInt64Ty());
    auto tagVal = builder_.CreateCall(getTag, {}, "exc.tag");
    auto dataVal = builder_.CreateCall(getData, {}, "exc.data");
    llvm::Value* caught = llvm::UndefValue::get(valueType_);
    caught = builder_.CreateInsertValue(caught, tagVal, {0});
    caught = builder_.CreateInsertValue(caught, dataVal, {1});
    builder_.CreateStore(caught, caughtSlot);
    builder_.CreateBr(catchBB);

    // --- Catch body: bind thrown value as `name` ---
    builder_.SetInsertPoint(catchBB);
    push_scope();
    auto caughtName = std::string(ast.nodes[1]->token);
    // Match interp's bind_pattern_name (interpreter.h:5264 → mut=true
    // default): the `catch e { ... }` binding is mutable so handlers
    // can do `catch e { e = transformed(e); ... }`.
    auto caughtValue = builder_.CreateLoad(valueType_, caughtSlot);
    declare_local(caughtName, caughtValue, /*is_mut=*/true);
    auto catchVal = compile(*ast.nodes[2]);
    pop_scope();
    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateStore(catchVal, resultSlot);
      builder_.CreateBr(endBB);
    }

    // --- End ---
    builder_.SetInsertPoint(endBB);
    return builder_.CreateLoad(valueType_, resultSlot, "try.res");
  }

  // --- Execution ---

  static void exec(std::unique_ptr<llvm::LLVMContext> ctx,
                   std::unique_ptr<llvm::Module> mod) {
    using namespace llvm;
    auto jit = create_jit_instance();
    orc::ThreadSafeContext tsctx(std::move(ctx));
    cantFail(jit->addIRModule(
        orc::ThreadSafeModule(std::move(mod), std::move(tsctx))));

    auto mainFn =
        cantFail(jit->lookup("__culebra_main")).toPtr<void (*)()>();
    // Force a cycle collect while the LLJIT (and therefore every
    // closure's `fn_ptr`) is still alive — on BOTH success and
    // throw paths. Without this, top-level object cycles that carry
    // a `drop` survive until ~_GcTracker runs at process exit; by
    // then the JIT module has been torn down and the drop closure's
    // native code is dangling, which segfaults. RAII guard covers
    // the throw-rethrow branch too (the catch below converts the
    // CulebraException to std::runtime_error, which unwinds before
    // any plain trailing statement would run).
    struct CollectGuard {
      ~CollectGuard() { _GcTracker::instance().collect(); }
    } collect_guard;
    try {
      mainFn();
    } catch (const CulebraException& e) {
      // Balance the retain performed in culebra_runtime_throw.
      _culebra_value_release_impl(e.tag, e.data);
      auto s = _culebra_value_to_str_impl(e.tag, e.data);
      // Run (best-effort) any top-level defers the uncaught throw
      // skipped, so the global defer stack is drained between runs.
      try {
        culebra_runtime_defer_run_to(0);
      } catch (...) {}
      throw std::runtime_error(std::format("uncaught: {}", s));
    }
  }
};

// Recognise an HOF callback that we can inline: the call site must
// pass a literal `|x| ...` (LAMBDA) or `fn (x) { ... }` (FUNCTION),
// with the expected number of params and no parameter defaults. The
// FuncInfo lookup ensures `analyze_function` already ran and we can
// honour the lambda's `captured_locals` decisions.
inline bool JIT::is_inlinable_lambda(const peg::Ast& ast,
                                      size_t expected_arity) const {
  using namespace peg::udl;
  if (ast.tag != "LAMBDA"_ && ast.tag != "FUNCTION"_) return false;
  if (ast.nodes.size() < 2) return false;
  const auto& params = *ast.nodes[0];
  // Inlined calls hand args positionally only — either `*` separator
  // or `**rest` catch-all forces the slow path.
  for (const auto& p : params.nodes) {
    if (culebra::is_kw_only_sep(*p) || culebra::is_kwargs_rest(*p)) {
      return false;
    }
  }
  if (params.nodes.size() != expected_arity) return false;
  for (const auto& p : params.nodes) {
    // PARAMETER: [MUTABLE, IDENTIFIER, (TYPE)?, (DEFAULT)?]. Any node
    // tagged DEFAULT_VALUE means we'd need full call-site dispatch
    // logic — not worth duplicating from compile_fn_common; fall back
    // to the runtime helper for those.
    for (const auto& sub : p->nodes) {
      if (sub->tag == "DEFAULT_VALUE"_) return false;
    }
  }
  return func_info_.count(&ast) > 0;
}

// Shared loop scaffold for unary inlined HOFs (map / filter / for_each).
// Builds a counted loop over `arrPtr`, binds the lambda's single param
// to each element in a fresh scope, compiles the body in place, then
// hands the +1-owned result to `per_iter` (which decides what to do
// with it: push to output, conditional push, discard, etc.). The
// `elem` value passed alongside is the current element — also +1
// retained — so per_iter can inspect or use it without a separate
// load. After per_iter returns, the scope pops (releasing the param
// slot's retained ref) and the loop advances.
//
// Adding a new unary HOF reduces to writing a per_iter body. See
// `emit_inlined_array_map` / `_filter` / `_for_each` for examples.
template <class PerIter>
inline void JIT::emit_array_unary_inline_loop(
    llvm::Value* arrPtr, const peg::Ast& lambda_ast,
    PerIter&& per_iter) {
  using namespace llvm;
  auto ptrTy = PointerType::get(ctx_, 0);
  auto fn = builder_.GetInsertBlock()->getParent();

  auto size = emit_call(
      module_->getOrInsertFunction(rt::array_size, builder_.getInt64Ty(),
                                   ptrTy),
      {arrPtr}, "ihof.size");

  IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
  auto idxAlloca =
      entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "ihof.idx");
  auto outTagAlloca =
      entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "ihof.elem.tag");
  auto outDataAlloca =
      entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "ihof.elem.data");
  builder_.CreateStore(builder_.getInt64(0), idxAlloca);

  auto condBB = BasicBlock::Create(ctx_, "ihof.cond", fn);
  auto bodyBB = BasicBlock::Create(ctx_, "ihof.body", fn);
  auto incBB = BasicBlock::Create(ctx_, "ihof.inc", fn);
  auto endBB = BasicBlock::Create(ctx_, "ihof.end", fn);

  builder_.CreateBr(condBB);

  builder_.SetInsertPoint(condBB);
  auto i = builder_.CreateLoad(builder_.getInt64Ty(), idxAlloca, "ihof.i");
  builder_.CreateCondBr(builder_.CreateICmpSLT(i, size), bodyBB, endBB);

  builder_.SetInsertPoint(bodyBB);
  emit_call(
      module_->getOrInsertFunction(
          rt::array_get, builder_.getVoidTy(), ptrTy,
          builder_.getInt64Ty(), ptrTy, ptrTy, builder_.getInt64Ty(),
          builder_.getInt64Ty()),
      {arrPtr, i, outTagAlloca, outDataAlloca, current_line_val(),
       current_column_val()});
  auto t = builder_.CreateLoad(builder_.getInt8Ty(), outTagAlloca);
  auto d = builder_.CreateLoad(builder_.getInt64Ty(), outDataAlloca);
  llvm::Value* elem = UndefValue::get(valueType_);
  elem = builder_.CreateInsertValue(elem, t, {0});
  elem = builder_.CreateInsertValue(elem, d, {1});

  emit_value_retain(elem);
  emit_unary_lambda_body(lambda_ast, elem, std::forward<PerIter>(per_iter));

  if (!builder_.GetInsertBlock()->getTerminator()) {
    builder_.CreateBr(incBB);
  }

  builder_.SetInsertPoint(incBB);
  auto next = builder_.CreateAdd(
      builder_.CreateLoad(builder_.getInt64Ty(), idxAlloca),
      builder_.getInt64(1));
  builder_.CreateStore(next, idxAlloca);
  builder_.CreateBr(condBB);

  builder_.SetInsertPoint(endBB);
}

inline llvm::Value* JIT::emit_inlined_array_map(
    llvm::Value* arrPtr, const peg::Ast& lambda_ast) {
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto size = emit_call(
      module_->getOrInsertFunction(rt::array_size, builder_.getInt64Ty(),
                                   ptrTy),
      {arrPtr}, "imap.size");
  auto out =
      emit_call(module_->getFunction(rt::array_new_reserved), {size});
  emit_array_unary_inline_loop(
      arrPtr, lambda_ast,
      [&](llvm::Value* /*elem*/, llvm::Value* result) {
        emit_call(
            module_->getOrInsertFunction(
                rt::array_push, builder_.getVoidTy(), ptrTy,
                builder_.getInt8Ty(), builder_.getInt64Ty()),
            {out, extract_tag(result), extract_data(result)});
      });
  return make_array(out);
}

// Inlined `arr.filter(|x| pred)`. Predicate result must be Bool-like
// (Bool / Long / Nil); non-bool/-numeric tags fall back to "drop"
// to mirror the runtime helper's `keep` rule (see culebra_runtime_array_filter).
inline llvm::Value* JIT::emit_inlined_array_filter(
    llvm::Value* arrPtr, const peg::Ast& lambda_ast) {
  using namespace llvm;
  auto ptrTy = PointerType::get(ctx_, 0);
  auto fn = builder_.GetInsertBlock()->getParent();
  auto out = emit_call(module_->getFunction(rt::array_new), {});
  emit_array_unary_inline_loop(
      arrPtr, lambda_ast,
      [&](llvm::Value* elem, llvm::Value* result) {
        auto rt_tag = extract_tag(result);
        auto rt_data = extract_data(result);
        auto isBoolish = builder_.CreateOr(
            builder_.CreateICmpEQ(rt_tag, builder_.getInt8(TAG_BOOL)),
            builder_.CreateICmpEQ(rt_tag, builder_.getInt8(TAG_LONG)));
        auto truthy = builder_.CreateAnd(
            isBoolish,
            builder_.CreateICmpNE(rt_data, builder_.getInt64(0)));
        auto keepBB = BasicBlock::Create(ctx_, "ifil.keep", fn);
        auto dropBB = BasicBlock::Create(ctx_, "ifil.drop", fn);
        auto contBB = BasicBlock::Create(ctx_, "ifil.cont", fn);
        builder_.CreateCondBr(truthy, keepBB, dropBB);

        builder_.SetInsertPoint(keepBB);
        emit_value_release(result);
        emit_value_retain(elem);
        emit_call(
            module_->getOrInsertFunction(
                rt::array_push, builder_.getVoidTy(), ptrTy,
                builder_.getInt8Ty(), builder_.getInt64Ty()),
            {out, extract_tag(elem), extract_data(elem)});
        builder_.CreateBr(contBB);

        builder_.SetInsertPoint(dropBB);
        emit_value_release(result);
        builder_.CreateBr(contBB);

        builder_.SetInsertPoint(contBB);
      });
  return make_array(out);
}

inline llvm::Value* JIT::emit_inlined_array_for_each(
    llvm::Value* arrPtr, const peg::Ast& lambda_ast) {
  emit_array_unary_inline_loop(
      arrPtr, lambda_ast,
      [&](llvm::Value* /*elem*/, llvm::Value* result) {
        emit_value_release(result);
      });
  return make_nil();
}

// Inlined `arr.reduce(init, |acc, val| body)`. Two-parameter lambda;
// the accumulator slot lives outside the loop and is overwritten with
// each iteration's body result. `init` is +1-owned by the caller and
// is moved into the accumulator (no extra retain/release).
inline llvm::Value* JIT::emit_inlined_array_reduce(
    llvm::Value* arrPtr, llvm::Value* init, const peg::Ast& lambda_ast) {
  using namespace llvm;
  auto ptrTy = PointerType::get(ctx_, 0);
  auto fn = builder_.GetInsertBlock()->getParent();

  // Accumulator alloca outside the loop. Lives in entry block to keep
  // it out of the loop hot path.
  IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
  auto accAlloca = entryB.CreateAlloca(valueType_, nullptr, "ired.acc");
  builder_.CreateStore(init, accAlloca);

  auto size = emit_call(
      module_->getOrInsertFunction(rt::array_size, builder_.getInt64Ty(),
                                   ptrTy),
      {arrPtr}, "ired.size");

  auto idxAlloca =
      entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "ired.idx");
  auto eTagAlloca =
      entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "ired.elem.tag");
  auto eDataAlloca =
      entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "ired.elem.data");
  builder_.CreateStore(builder_.getInt64(0), idxAlloca);

  auto condBB = BasicBlock::Create(ctx_, "ired.cond", fn);
  auto bodyBB = BasicBlock::Create(ctx_, "ired.body", fn);
  auto incBB = BasicBlock::Create(ctx_, "ired.inc", fn);
  auto endBB = BasicBlock::Create(ctx_, "ired.end", fn);

  builder_.CreateBr(condBB);

  builder_.SetInsertPoint(condBB);
  auto i = builder_.CreateLoad(builder_.getInt64Ty(), idxAlloca, "ired.i");
  builder_.CreateCondBr(builder_.CreateICmpSLT(i, size), bodyBB, endBB);

  builder_.SetInsertPoint(bodyBB);
  emit_call(
      module_->getOrInsertFunction(
          rt::array_get, builder_.getVoidTy(), ptrTy,
          builder_.getInt64Ty(), ptrTy, ptrTy, builder_.getInt64Ty(),
          builder_.getInt64Ty()),
      {arrPtr, i, eTagAlloca, eDataAlloca, current_line_val(),
       current_column_val()});
  auto t = builder_.CreateLoad(builder_.getInt8Ty(), eTagAlloca);
  auto d = builder_.CreateLoad(builder_.getInt64Ty(), eDataAlloca);
  llvm::Value* elem = UndefValue::get(valueType_);
  elem = builder_.CreateInsertValue(elem, t, {0});
  elem = builder_.CreateInsertValue(elem, d, {1});
  emit_value_retain(elem);

  // Move the current acc out of its alloca for the body. The body's
  // result is the new acc; store it back. No retain/release dance —
  // the alloca's value is always +1 owned, ownership transfers
  // through the body as if the runtime helper called `_culebra_invoke2`.
  auto curAcc = builder_.CreateLoad(valueType_, accAlloca, "ired.acc.cur");

  emit_binary_lambda_body(
      lambda_ast, curAcc, elem,
      [&](llvm::Value* result) { builder_.CreateStore(result, accAlloca); });

  if (!builder_.GetInsertBlock()->getTerminator()) {
    builder_.CreateBr(incBB);
  }

  builder_.SetInsertPoint(incBB);
  auto next = builder_.CreateAdd(
      builder_.CreateLoad(builder_.getInt64Ty(), idxAlloca),
      builder_.getInt64(1));
  builder_.CreateStore(next, idxAlloca);
  builder_.CreateBr(condBB);

  builder_.SetInsertPoint(endBB);
  return builder_.CreateLoad(valueType_, accAlloca, "ired.result");
}

// Shared loop scaffold for unary inlined Iterator HOFs. Reads the
// iterator's `next` closure once outside the loop, then steps via
// `culebra_runtime_iter_advance` (mirroring the for-in protocol
// loop). `per_iter` runs after the body compiles, with the lambda
// param's slot still bound.
template <class PerIter>
inline void JIT::emit_iter_unary_inline_loop(
    llvm::Value* iter_val, const peg::Ast& lambda_ast, PerIter&& per_iter) {
  using namespace llvm;
  auto ptrTy = PointerType::get(ctx_, 0);
  auto fn = builder_.GetInsertBlock()->getParent();

  auto has_next_fn_val = compile_property_get(iter_val, "has_next");
  auto has_next_cls_ptr = builder_.CreateIntToPtr(
      extract_data(has_next_fn_val), ptrTy, "ihof.has_next.cls");
  auto next_fn_val = compile_property_get(iter_val, "next");
  auto next_cls_ptr =
      builder_.CreateIntToPtr(extract_data(next_fn_val), ptrTy, "ihof.next.cls");
  auto iterTag = extract_tag(iter_val);
  auto iterData = extract_data(iter_val);

  IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
  auto outTagAlloca =
      entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "ihof.it.tag");
  auto outDataAlloca =
      entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "ihof.it.data");

  auto condBB = BasicBlock::Create(ctx_, "ihofi.cond", fn);
  auto bodyBB = BasicBlock::Create(ctx_, "ihofi.body", fn);
  auto endBB = BasicBlock::Create(ctx_, "ihofi.end", fn);

  builder_.CreateBr(condBB);

  builder_.SetInsertPoint(condBB);
  auto ok = emit_call(
      module_->getFunction(rt::iter_advance),
      {has_next_cls_ptr, next_cls_ptr, iterTag, iterData,
       outTagAlloca, outDataAlloca},
      "ihofi.ok");
  auto alive =
      builder_.CreateICmpNE(ok, builder_.getInt64(0), "ihofi.alive");
  builder_.CreateCondBr(alive, bodyBB, endBB);

  builder_.SetInsertPoint(bodyBB);
  auto t = builder_.CreateLoad(builder_.getInt8Ty(), outTagAlloca);
  auto d = builder_.CreateLoad(builder_.getInt64Ty(), outDataAlloca);
  llvm::Value* elem = UndefValue::get(valueType_);
  elem = builder_.CreateInsertValue(elem, t, {0});
  elem = builder_.CreateInsertValue(elem, d, {1});
  // iter_advance returns a +1-owned Value; we hand that ref to the
  // param slot directly (no extra retain).
  emit_unary_lambda_body(lambda_ast, elem, std::forward<PerIter>(per_iter));

  if (!builder_.GetInsertBlock()->getTerminator()) {
    builder_.CreateBr(condBB);
  }

  builder_.SetInsertPoint(endBB);
}

inline llvm::Value* JIT::emit_inlined_iter_for_each(
    llvm::Value* iter_val, const peg::Ast& lambda_ast) {
  emit_iter_unary_inline_loop(
      iter_val, lambda_ast,
      [&](llvm::Value* /*elem*/, llvm::Value* result) {
        emit_value_release(result);
      });
  return make_nil();
}

inline std::optional<llvm::Value*> JIT::try_fuse_iter_map_collect(
    const peg::Ast& ast, size_t i, llvm::Value* receiver) {
  using namespace peg::udl;
  // Postfix shape: DOT(map) ARGUMENTS(λ) DOT(collect) ARGUMENTS().
  // Caller has already verified ast.nodes[i] = DOT and
  // ast.nodes[i+1] = ARGUMENTS, and that postfix.token == "map".
  if (ast.nodes[i + 1]->nodes.size() != 1) return std::nullopt;
  if (!is_inlinable_lambda(*ast.nodes[i + 1]->nodes[0], 1)) {
    return std::nullopt;
  }
  if (i + 3 >= ast.nodes.size()) return std::nullopt;
  if (ast.nodes[i + 2]->original_tag != "DOT"_) return std::nullopt;
  if (ast.nodes[i + 2]->token != "collect") return std::nullopt;
  if (ast.nodes[i + 3]->original_tag != "ARGUMENTS"_) return std::nullopt;
  if (!ast.nodes[i + 3]->nodes.empty()) return std::nullopt;
  expect_tag(receiver, TAG_OBJECT, "map.collect");
  return emit_inlined_iter_map_collect(receiver,
                                        *ast.nodes[i + 1]->nodes[0]);
}

inline llvm::Value* JIT::emit_inlined_iter_map_collect(
    llvm::Value* iter_val, const peg::Ast& lambda_ast) {
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  // Allocate the destination Array up-front; per-element results get
  // pushed into it inside the inlined iteration loop.
  auto outPtr = emit_call(
      module_->getOrInsertFunction(rt::array_new, ptrTy), {}, "imc.out");
  emit_iter_unary_inline_loop(
      iter_val, lambda_ast,
      [&](llvm::Value* /*elem*/, llvm::Value* result) {
        emit_call(
            module_->getOrInsertFunction(
                rt::array_push, builder_.getVoidTy(), ptrTy,
                builder_.getInt8Ty(), builder_.getInt64Ty()),
            {outPtr, extract_tag(result), extract_data(result)});
      });
  return make_array(outPtr);
}

// Shared scaffold for `Math.range(N).<unary HOF>(λ)`: emits an i64
// counter loop, binds `λ`'s single param to `i` in a fresh scope,
// compiles the body inline, then hands the +1-owned result to
// `per_iter`. Skips the per-call iterator-wrapper allocation that
// `culebra_runtime_math_range` does (and the per-element trampoline
// closure call).
template <class PerIter>
inline void JIT::emit_range_unary_inline_loop(const peg::Ast& n_ast,
                                               const peg::Ast& lambda_ast,
                                               PerIter&& per_iter) {
  using namespace llvm;
  auto fn = builder_.GetInsertBlock()->getParent();
  auto i64Ty = builder_.getInt64Ty();

  auto n_val = value_to_long(compile(n_ast));

  IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
  auto iAlloca = entryB.CreateAlloca(i64Ty, nullptr, "rng.i");
  builder_.CreateStore(builder_.getInt64(0), iAlloca);

  auto condBB = BasicBlock::Create(ctx_, "rng.cond", fn);
  auto bodyBB = BasicBlock::Create(ctx_, "rng.body", fn);
  auto endBB = BasicBlock::Create(ctx_, "rng.end", fn);
  builder_.CreateBr(condBB);

  builder_.SetInsertPoint(condBB);
  auto i_cond = builder_.CreateLoad(i64Ty, iAlloca, "rng.i.cur");
  auto cond = builder_.CreateICmpSLT(i_cond, n_val, "rng.cond.lt");
  builder_.CreateCondBr(cond, bodyBB, endBB);

  builder_.SetInsertPoint(bodyBB);
  auto i_cur = builder_.CreateLoad(i64Ty, iAlloca, "rng.i.body");
  auto i_val = make_long(i_cur);

  emit_unary_lambda_body(lambda_ast, i_val, std::forward<PerIter>(per_iter));

  if (!builder_.GetInsertBlock()->getTerminator()) {
    auto i_next =
        builder_.CreateAdd(i_cur, builder_.getInt64(1), "rng.i.next");
    builder_.CreateStore(i_next, iAlloca);
    builder_.CreateBr(condBB);
  }

  builder_.SetInsertPoint(endBB);
}

inline llvm::Value* JIT::emit_inlined_range_for_each(
    const peg::Ast& n_ast, const peg::Ast& lambda_ast) {
  emit_range_unary_inline_loop(
      n_ast, lambda_ast,
      [&](llvm::Value* /*i*/, llvm::Value* result) {
        emit_value_release(result);
      });
  return make_nil();
}

inline llvm::Value* JIT::emit_inlined_range_map_collect(
    const peg::Ast& n_ast, const peg::Ast& lambda_ast) {
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto outPtr = emit_call(
      module_->getOrInsertFunction(rt::array_new, ptrTy), {}, "rmc.out");
  emit_range_unary_inline_loop(
      n_ast, lambda_ast,
      [&](llvm::Value* /*i*/, llvm::Value* result) {
        emit_call(
            module_->getOrInsertFunction(
                rt::array_push, builder_.getVoidTy(), ptrTy,
                builder_.getInt8Ty(), builder_.getInt64Ty()),
            {outPtr, extract_tag(result), extract_data(result)});
      });
  return make_array(outPtr);
}

inline llvm::Value* JIT::emit_inlined_range_reduce(
    const peg::Ast& n_ast, const peg::Ast& init_ast,
    const peg::Ast& lambda_ast) {
  using namespace llvm;
  auto fn = builder_.GetInsertBlock()->getParent();
  auto i64Ty = builder_.getInt64Ty();

  // acc alloca seeded with init's +1.
  IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
  auto accAlloca = entryB.CreateAlloca(valueType_, nullptr, "rrd.acc");
  builder_.CreateStore(compile(init_ast), accAlloca);

  auto n_val = value_to_long(compile(n_ast));
  auto iAlloca = entryB.CreateAlloca(i64Ty, nullptr, "rrd.i");
  builder_.CreateStore(builder_.getInt64(0), iAlloca);

  auto condBB = BasicBlock::Create(ctx_, "rrd.cond", fn);
  auto bodyBB = BasicBlock::Create(ctx_, "rrd.body", fn);
  auto endBB = BasicBlock::Create(ctx_, "rrd.end", fn);
  builder_.CreateBr(condBB);

  builder_.SetInsertPoint(condBB);
  auto i_cond = builder_.CreateLoad(i64Ty, iAlloca, "rrd.i.cur");
  auto cond = builder_.CreateICmpSLT(i_cond, n_val, "rrd.cond.lt");
  builder_.CreateCondBr(cond, bodyBB, endBB);

  builder_.SetInsertPoint(bodyBB);
  auto i_cur = builder_.CreateLoad(i64Ty, iAlloca, "rrd.i.body");
  auto i_val = make_long(i_cur);
  auto acc_val = builder_.CreateLoad(valueType_, accAlloca, "rrd.acc.cur");

  emit_binary_lambda_body(
      lambda_ast, acc_val, i_val,
      [&](llvm::Value* result) {
        // Release the prior acc before overwriting (mirrors
        // `emit_inlined_iter_reduce`).
        auto old_acc =
            builder_.CreateLoad(valueType_, accAlloca, "rrd.acc.prev");
        emit_value_release(old_acc);
        builder_.CreateStore(result, accAlloca);
      });

  if (!builder_.GetInsertBlock()->getTerminator()) {
    auto i_next =
        builder_.CreateAdd(i_cur, builder_.getInt64(1), "rrd.i.next");
    builder_.CreateStore(i_next, iAlloca);
    builder_.CreateBr(condBB);
  }

  builder_.SetInsertPoint(endBB);
  return builder_.CreateLoad(valueType_, accAlloca, "rrd.acc.final");
}


inline llvm::Value* JIT::emit_inlined_iter_reduce(
    llvm::Value* iter_val, llvm::Value* init, const peg::Ast& lambda_ast) {
  using namespace llvm;
  auto ptrTy = PointerType::get(ctx_, 0);
  auto fn = builder_.GetInsertBlock()->getParent();

  IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
  auto accAlloca = entryB.CreateAlloca(valueType_, nullptr, "iri.acc");
  builder_.CreateStore(init, accAlloca);

  auto has_next_fn_val = compile_property_get(iter_val, "has_next");
  auto has_next_cls_ptr = builder_.CreateIntToPtr(
      extract_data(has_next_fn_val), ptrTy, "iri.has_next.cls");
  auto next_fn_val = compile_property_get(iter_val, "next");
  auto next_cls_ptr =
      builder_.CreateIntToPtr(extract_data(next_fn_val), ptrTy, "iri.next.cls");
  auto iterTag = extract_tag(iter_val);
  auto iterData = extract_data(iter_val);

  auto outTagAlloca =
      entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "iri.it.tag");
  auto outDataAlloca =
      entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "iri.it.data");

  auto condBB = BasicBlock::Create(ctx_, "iri.cond", fn);
  auto bodyBB = BasicBlock::Create(ctx_, "iri.body", fn);
  auto endBB = BasicBlock::Create(ctx_, "iri.end", fn);

  builder_.CreateBr(condBB);

  builder_.SetInsertPoint(condBB);
  auto ok = emit_call(
      module_->getFunction(rt::iter_advance),
      {has_next_cls_ptr, next_cls_ptr, iterTag, iterData,
       outTagAlloca, outDataAlloca},
      "iri.ok");
  auto alive = builder_.CreateICmpNE(ok, builder_.getInt64(0), "iri.alive");
  builder_.CreateCondBr(alive, bodyBB, endBB);

  builder_.SetInsertPoint(bodyBB);
  auto t = builder_.CreateLoad(builder_.getInt8Ty(), outTagAlloca);
  auto d = builder_.CreateLoad(builder_.getInt64Ty(), outDataAlloca);
  llvm::Value* elem = UndefValue::get(valueType_);
  elem = builder_.CreateInsertValue(elem, t, {0});
  elem = builder_.CreateInsertValue(elem, d, {1});

  // Move acc out of the alloca into the body's scope (same ownership
  // dance as emit_inlined_array_reduce). iter_advance handed elem in
  // +1 so val slot can absorb it directly.
  auto curAcc = builder_.CreateLoad(valueType_, accAlloca, "iri.acc.cur");

  emit_binary_lambda_body(
      lambda_ast, curAcc, elem,
      [&](llvm::Value* result) { builder_.CreateStore(result, accAlloca); });

  if (!builder_.GetInsertBlock()->getTerminator()) {
    builder_.CreateBr(condBB);
  }

  builder_.SetInsertPoint(endBB);
  return builder_.CreateLoad(valueType_, accAlloca, "iri.result");
}

inline llvm::Value* JIT::compile_builtin_method(const std::string& method,
                                                const peg::Ast& argsAst,
                                                llvm::Value* receiver) {
  // Fast path: bail out immediately on unknown method names before doing any
  // setup. Keeps user-defined method calls free of builtin-method overhead.
  if (!known_builtin_methods().contains(method)) return nullptr;

  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto tag = extract_tag(receiver);
  auto fn = builder_.GetInsertBlock()->getParent();

  // Tensor methods. Dispatch unconditionally on TAG_TENSOR; non-Tensor
  // receivers raise a type error (no overload with other types in M1).
  if (method == "shape" && argsAst.nodes.size() == 0) {
    auto tPtr = expect_tag(receiver, TAG_TENSOR, "shape");
    auto arrPtr = emit_call(
        module_->getFunction(rt::tensor_shape), {tPtr}, "tshape");
    return make_array(arrPtr);
  }
  if (method == "pow" && argsAst.nodes.size() == 1) {
    expect_tag(receiver, TAG_TENSOR, "pow");
    auto exp = compile(*argsAst.nodes[0]);
    auto resultPtr = emit_call(
        module_->getFunction(rt::tensor_binop),
        {extract_tag(receiver), extract_data(receiver),
         extract_tag(exp), extract_data(exp),
         builder_.getInt64(static_cast<int64_t>(culebra::Op::Pow))},
        "tpow");
    emit_value_release(exp);
    return make_tensor(resultPtr);
  }
  if (method == "transpose" && argsAst.nodes.size() == 0) {
    auto tPtr = expect_tag(receiver, TAG_TENSOR, "transpose");
    auto resultPtr = emit_call(
        module_->getFunction(rt::tensor_transpose), {tPtr}, "tt");
    return make_tensor(resultPtr);
  }
  if (method == "clone" && argsAst.nodes.size() == 0) {
    auto tPtr = expect_tag(receiver, TAG_TENSOR, "clone");
    auto resultPtr = emit_call(
        module_->getFunction(rt::tensor_clone), {tPtr}, "tcl");
    return make_tensor(resultPtr);
  }
  if (method == "reshape" && argsAst.nodes.size() == 1) {
    auto tPtr = expect_tag(receiver, TAG_TENSOR, "reshape");
    auto dims = compile(*argsAst.nodes[0]);
    emit_type_check(dims, "Array", "Tensor.reshape argument");
    auto dimsPtr = builder_.CreateIntToPtr(extract_data(dims), ptrTy);
    auto resultPtr = emit_call(
        module_->getFunction(rt::tensor_reshape), {tPtr, dimsPtr}, "tr");
    emit_value_release(dims);
    return make_tensor(resultPtr);
  }

  // Tensor axis-ful reductions (1 Long arg). Tensor-only — receivers
  // of other types fall through to the existing Array `.sum() / .max()`
  // handler below (which only accepts 0 args, raising type_error here).
  if ((method == "sum" || method == "mean" || method == "max" ||
       method == "argmax") && argsAst.nodes.size() == 1) {
    auto tPtr = expect_tag(receiver, TAG_TENSOR, method.c_str());
    auto axis = value_to_long(compile(*argsAst.nodes[0]));
    int op_id =
        method == "sum"    ? static_cast<int>(culebra::Op::Sum)
      : method == "mean"   ? static_cast<int>(culebra::Op::Mean)
      : method == "max"    ? static_cast<int>(culebra::Op::Max)
                           : static_cast<int>(culebra::Op::Argmax);
    auto resultPtr = emit_call(
        module_->getFunction(rt::tensor_reduce_axis),
        {tPtr, builder_.getInt64(op_id), axis}, "trax");
    return make_tensor(resultPtr);
  }
  // Tensor `.mean()` (0-arg). `.sum()` / `.max()` 0-arg fall through
  // to the polymorphic handler below which adds a TAG_TENSOR branch.
  if (method == "mean" && argsAst.nodes.size() == 0) {
    auto tPtr = expect_tag(receiver, TAG_TENSOR, "mean");
    auto v = emit_call(
        module_->getFunction(rt::tensor_reduce_all),
        {tPtr, builder_.getInt64(static_cast<int>(culebra::Op::Mean))},
        "trall");
    return v;
  }
  if (method == "to_array" && argsAst.nodes.size() == 0) {
    auto tenBB = llvm::BasicBlock::Create(ctx_, "ta.ten", fn);
    auto tupBB = llvm::BasicBlock::Create(ctx_, "ta.tup", fn);
    auto setBB = llvm::BasicBlock::Create(ctx_, "ta.set", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "ta.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "ta.merge", fn);
    auto sw = builder_.CreateSwitch(tag, errBB, 3);
    sw->addCase(builder_.getInt8(TAG_TENSOR), tenBB);
    sw->addCase(builder_.getInt8(TAG_TUPLE), tupBB);
    sw->addCase(builder_.getInt8(TAG_SET), setBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("Tensor, Tuple, or Set", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(tenBB);
    auto tenPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto tenArr = emit_call(
        module_->getFunction(rt::tensor_to_array), {tenPtr}, "tta");
    auto tenVal = make_array(tenArr);
    auto tenEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(tupBB);
    auto tupPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto tupArr = emit_call(
        module_->getOrInsertFunction(rt::tuple_to_array, ptrTy, ptrTy),
        {tupPtr}, "ta.tup.arr");
    auto tupVal = make_array(tupArr);
    auto tupEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(setBB);
    auto setPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto setArr = emit_call(
        module_->getOrInsertFunction(rt::set_to_array, ptrTy, ptrTy),
        {setPtr}, "ta.set.arr");
    auto setVal = make_array(setArr);
    auto setEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 3, "ta.r");
    phi->addIncoming(tenVal, tenEnd);
    phi->addIncoming(tupVal, tupEnd);
    phi->addIncoming(setVal, setEnd);
    return phi;
  }
  if (method == "dot" && argsAst.nodes.size() == 1) {
    auto aPtr = expect_tag(receiver, TAG_TENSOR, "dot");
    auto other = compile(*argsAst.nodes[0]);
    emit_type_check(other, "Tensor", "Tensor.dot argument");
    auto bPtr = builder_.CreateIntToPtr(extract_data(other), ptrTy);
    auto resultPtr = emit_call(
        module_->getFunction(rt::tensor_dot), {aPtr, bPtr}, "td");
    emit_value_release(other);
    return make_tensor(resultPtr);
  }
  if (method == "linear_sigmoid" && argsAst.nodes.size() == 2) {
    auto wPtr = expect_tag(receiver, TAG_TENSOR, "linear_sigmoid");
    auto xv = compile(*argsAst.nodes[0]);
    emit_type_check(xv, "Tensor", "linear_sigmoid x");
    auto bv = compile(*argsAst.nodes[1]);
    emit_type_check(bv, "Tensor", "linear_sigmoid b");
    auto xp = builder_.CreateIntToPtr(extract_data(xv), ptrTy);
    auto bp = builder_.CreateIntToPtr(extract_data(bv), ptrTy);
    auto resultPtr = emit_call(
        module_->getFunction(rt::tensor_linear_sigmoid),
        {wPtr, xp, bp}, "tls");
    emit_value_release(xv);
    emit_value_release(bv);
    return make_tensor(resultPtr);
  }

  // .size() — works on Array, Object, String, StringView, Set
  if (method == "size" && argsAst.nodes.size() == 0) {
    auto arrBB = llvm::BasicBlock::Create(ctx_, "size.arr", fn);
    auto objBB = llvm::BasicBlock::Create(ctx_, "size.obj", fn);
    auto strBB = llvm::BasicBlock::Create(ctx_, "size.str", fn);
    auto svBB  = llvm::BasicBlock::Create(ctx_, "size.sv", fn);
    auto setBB = llvm::BasicBlock::Create(ctx_, "size.set", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "size.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "size.merge", fn);

    auto sw = builder_.CreateSwitch(tag, errBB, 6);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrBB);
    sw->addCase(builder_.getInt8(TAG_TUPLE), arrBB);
    sw->addCase(builder_.getInt8(TAG_OBJECT), objBB);
    sw->addCase(builder_.getInt8(TAG_STRING), strBB);
    sw->addCase(builder_.getInt8(TAG_STRINGVIEW), svBB);
    sw->addCase(builder_.getInt8(TAG_SET), setBB);

    builder_.SetInsertPoint(arrBB);
    auto arrPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto arrSize = emit_call(
        module_->getFunction(rt::array_size), {arrPtr}, "asz");
    auto arrSizeBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(objBB);
    auto objPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto objSize = emit_call(
        module_->getFunction(rt::object_size), {objPtr}, "osz");
    auto objSizeBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(strBB);
    auto strPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto strSize = emit_call(
        module_->getFunction(rt::str_size), {strPtr}, "ssz");
    auto strSizeBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    // JitStringView { ptr, len } — load len at offset 8.
    builder_.SetInsertPoint(svBB);
    auto svPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto svLenPtr = builder_.CreateConstInBoundsGEP1_64(
        builder_.getInt8Ty(), svPtr, 8);
    auto svSize = builder_.CreateLoad(builder_.getInt64Ty(), svLenPtr, "svsz");
    auto svSizeBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(setBB);
    auto setPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto setSize = emit_call(
        module_->getOrInsertFunction(rt::set_size,
                                     builder_.getInt64Ty(), ptrTy),
        {setPtr}, "ssz2");
    auto setSizeBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("Array, Object, String, or Set", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt64Ty(), 5, "sz");
    phi->addIncoming(arrSize, arrSizeBB);
    phi->addIncoming(objSize, objSizeBB);
    phi->addIncoming(strSize, strSizeBB);
    phi->addIncoming(svSize, svSizeBB);
    phi->addIncoming(setSize, setSizeBB);
    return make_long(phi);
  }

  // --- Set methods ---
  // union/intersect/diff/sym_diff: both operands Set, returns Set.
  if ((method == "union" || method == "intersect" ||
       method == "diff"  || method == "sym_diff") &&
      argsAst.nodes.size() == 1) {
    auto aPtr = expect_tag(receiver, TAG_SET, method.c_str());
    auto other = compile(*argsAst.nodes[0]);
    emit_type_check(other, "Set", "Set operation argument");
    auto bPtr = builder_.CreateIntToPtr(extract_data(other), ptrTy);
    const char* rt_name = method == "union"     ? rt::set_union
                        : method == "intersect" ? rt::set_intersect
                        : method == "diff"      ? rt::set_diff
                                                : rt::set_sym_diff;
    auto out = emit_call(
        module_->getOrInsertFunction(rt_name, ptrTy, ptrTy, ptrTy),
        {aPtr, bPtr}, "set.op");
    emit_value_release(other);
    return make_set(out);
  }
  // subset/superset: both operands Set, returns Bool.
  if ((method == "subset" || method == "superset") &&
      argsAst.nodes.size() == 1) {
    auto aPtr = expect_tag(receiver, TAG_SET, method.c_str());
    auto other = compile(*argsAst.nodes[0]);
    emit_type_check(other, "Set", "Set predicate argument");
    auto bPtr = builder_.CreateIntToPtr(extract_data(other), ptrTy);
    const char* rt_name = method == "subset" ? rt::set_subset
                                             : rt::set_superset;
    auto r = emit_call(
        module_->getOrInsertFunction(
            rt_name, builder_.getInt8Ty(), ptrTy, ptrTy),
        {aPtr, bPtr}, "set.pred");
    emit_value_release(other);
    return make_bool(builder_.CreateICmpNE(r, builder_.getInt8(0)));
  }
  // Set's mutating `.add(x)` / `.remove(x)` are routed through
  // `compile_set_mutate_dispatch` higher up in this file — that path
  // does a runtime tag dispatch so Set receivers hit the set primitive
  // and Object receivers fall through to a user-defined method.

  // --- Array methods ---

  if (method == "push" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "push");
    auto val = compile(*argsAst.nodes[0]);
    emit_call(module_->getFunction(rt::array_push),
                        {arrPtr, extract_tag(val), extract_data(val)});
    return make_nil();
  }

  if (method == "pop" && argsAst.nodes.size() == 0) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "pop");
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    auto outTag = entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "pop.tag");
    auto outData =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "pop.data");
    emit_call(module_->getFunction(rt::array_pop),
                        {arrPtr, outTag, outData});
    auto tagLoaded = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
    auto dataLoaded = builder_.CreateLoad(builder_.getInt64Ty(), outData);
    llvm::Value* result = llvm::UndefValue::get(valueType_);
    result = builder_.CreateInsertValue(result, tagLoaded, {0});
    result = builder_.CreateInsertValue(result, dataLoaded, {1});
    return result;
  }

  if (method == "reverse" && argsAst.nodes.size() == 0) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "rev");
    emit_call(module_->getFunction(rt::array_reverse),
                        {arrPtr});
    return make_nil();
  }

  // slice works on Array, String, StringView, and Tensor.
  // String/StringView return TAG_STRINGVIEW into the source bytes.
  if (method == "slice" && argsAst.nodes.size() == 2) {
    auto start = value_to_long(compile(*argsAst.nodes[0]));
    auto end = value_to_long(compile(*argsAst.nodes[1]));

    auto arrBB = llvm::BasicBlock::Create(ctx_, "sl.arr", fn);
    auto strBB = llvm::BasicBlock::Create(ctx_, "sl.str", fn);
    auto tenBB = llvm::BasicBlock::Create(ctx_, "sl.ten", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "sl.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "sl.merge", fn);

    auto sw = builder_.CreateSwitch(tag, errBB, 4);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrBB);
    sw->addCase(builder_.getInt8(TAG_STRING), strBB);
    sw->addCase(builder_.getInt8(TAG_STRINGVIEW), strBB);
    sw->addCase(builder_.getInt8(TAG_TENSOR), tenBB);

    builder_.SetInsertPoint(arrBB);
    auto arrPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto newArr = emit_call(
        module_->getFunction(rt::array_slice2),
        {arrPtr, start, end});
    auto arrVal = make_array(newArr);
    auto arrBBEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(strBB);
    auto newView = emit_call(
        module_->getFunction(rt::strlike_slice_view),
        {tag, extract_data(receiver), start, end});
    auto strVal = make_stringview(newView);
    auto strBBEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(tenBB);
    auto tenPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto newTen = emit_call(
        module_->getFunction(rt::tensor_slice),
        {tenPtr, start, end});
    auto tenVal = make_tensor(newTen);
    auto tenBBEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("Array, String, or Tensor", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 3, "sl");
    phi->addIncoming(arrVal, arrBBEnd);
    phi->addIncoming(strVal, strBBEnd);
    phi->addIncoming(tenVal, tenBBEnd);
    return phi;
  }

  if (method == "join" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "join");
    auto sep = compile(*argsAst.nodes[0]);
    emit_type_check(sep, "String", "join separator");
    auto sepPtr = builder_.CreateIntToPtr(extract_data(sep), ptrTy);
    auto s = emit_call(
        module_->getFunction(rt::array_join), {arrPtr, sepPtr});
    emit_value_release(sep);
    return make_string(s);
  }

  if (method == "index_of" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "iof");
    auto v = compile(*argsAst.nodes[0]);
    auto idx = emit_call(
        module_->getFunction(rt::array_index_of),
        {arrPtr, extract_tag(v), extract_data(v)});
    emit_value_release(v);
    return make_long(idx);
  }

  // contains works on Array, String, Set, and Tuple (different arg types).
  if (method == "contains" && argsAst.nodes.size() == 1) {
    auto arrBB = llvm::BasicBlock::Create(ctx_, "ct.arr", fn);
    auto strBB = llvm::BasicBlock::Create(ctx_, "ct.str", fn);
    auto setBB = llvm::BasicBlock::Create(ctx_, "ct.set", fn);
    auto tupBB = llvm::BasicBlock::Create(ctx_, "ct.tup", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "ct.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "ct.merge", fn);

    auto sw = builder_.CreateSwitch(tag, errBB, 5);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrBB);
    sw->addCase(builder_.getInt8(TAG_STRING), strBB);
    sw->addCase(builder_.getInt8(TAG_STRINGVIEW), strBB);
    sw->addCase(builder_.getInt8(TAG_SET), setBB);
    sw->addCase(builder_.getInt8(TAG_TUPLE), tupBB);

    builder_.SetInsertPoint(arrBB);
    auto arrPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto v = compile(*argsAst.nodes[0]);
    auto arrFound = emit_call(
        module_->getFunction(rt::array_contains),
        {arrPtr, extract_tag(v), extract_data(v)});
    emit_value_release(v);
    auto arrBoolVal = make_bool(arrFound);
    auto arrEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(strBB);
    auto strPtr = emit_call(module_->getFunction(rt::strlike_to_cstr),
                            {tag, extract_data(receiver)});
    auto sub = compile(*argsAst.nodes[0]);
    auto subPtr = coerce_strlike_cstr(sub, "ct.sub");
    auto strFound = emit_call(
        module_->getFunction(rt::str_contains),
        {strPtr, subPtr});
    emit_value_release(sub);
    auto strBoolVal = make_bool(strFound);
    auto strEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(setBB);
    auto setPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto vs = compile(*argsAst.nodes[0]);
    auto setFound = emit_call(
        module_->getOrInsertFunction(
            rt::set_contains, builder_.getInt1Ty(), ptrTy,
            builder_.getInt8Ty(), builder_.getInt64Ty()),
        {setPtr, extract_tag(vs), extract_data(vs)});
    emit_value_release(vs);
    auto setBoolVal = make_bool(setFound);
    auto setEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(tupBB);
    auto tupPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto vt = compile(*argsAst.nodes[0]);
    auto tupFound = emit_call(
        module_->getOrInsertFunction(
            rt::tuple_contains, builder_.getInt8Ty(), ptrTy,
            builder_.getInt8Ty(), builder_.getInt64Ty()),
        {tupPtr, extract_tag(vt), extract_data(vt)});
    emit_value_release(vt);
    auto tupBoolVal = make_bool(
        builder_.CreateICmpNE(tupFound, builder_.getInt8(0)));
    auto tupEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("Array, String, Set, or Tuple", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 4, "ct");
    phi->addIncoming(arrBoolVal, arrEnd);
    phi->addIncoming(strBoolVal, strEnd);
    phi->addIncoming(setBoolVal, setEnd);
    phi->addIncoming(tupBoolVal, tupEnd);
    return phi;
  }

  // --- String methods ---

  // `.view()` on String/StringView → TAG_STRINGVIEW.
  if (method == "view" && argsAst.nodes.size() == 0) {
    auto errBB = llvm::BasicBlock::Create(ctx_, "vw.err", fn);
    auto okBB = llvm::BasicBlock::Create(ctx_, "vw.ok", fn);
    auto sw = builder_.CreateSwitch(tag, errBB, 2);
    sw->addCase(builder_.getInt8(TAG_STRING), okBB);
    sw->addCase(builder_.getInt8(TAG_STRINGVIEW), okBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("String or StringView", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(okBB);
    auto v = emit_call(module_->getFunction(rt::strlike_view),
                       {tag, extract_data(receiver)});
    return make_stringview(v);
  }

  if (method == "upper" && argsAst.nodes.size() == 0) {
    auto strPtr = coerce_strlike_cstr(receiver, "up");
    auto s = emit_call(
        module_->getFunction(rt::str_upper), {strPtr});
    return make_string(s);
  }

  if (method == "lower" && argsAst.nodes.size() == 0) {
    auto strPtr = coerce_strlike_cstr(receiver, "lo");
    auto s = emit_call(
        module_->getFunction(rt::str_lower), {strPtr});
    return make_string(s);
  }

  if (method == "trim" && argsAst.nodes.size() == 0) {
    auto strPtr = coerce_strlike_cstr(receiver, "tr");
    auto s = emit_call(
        module_->getFunction(rt::str_trim), {strPtr});
    return make_string(s);
  }

  if (method == "split" && argsAst.nodes.size() == 1) {
    auto strPtr = coerce_strlike_cstr(receiver, "sp");
    auto sep = compile(*argsAst.nodes[0]);
    auto sepPtr = coerce_strlike_cstr(sep, "sp.sep");
    auto arr = emit_call(
        module_->getFunction(rt::str_split), {strPtr, sepPtr});
    emit_value_release(sep);
    return make_array(arr);
  }

  // split_iter is "lazy in API, eager underneath" for now: build the
  // Array via str_split, wrap it in array_iter so the iter protocol
  // composes. True lazy is a later refinement.
  if (method == "split_iter" && argsAst.nodes.size() == 1) {
    auto strPtr = coerce_strlike_cstr(receiver, "spli");
    auto sep = compile(*argsAst.nodes[0]);
    auto sepPtr = coerce_strlike_cstr(sep, "spli.sep");
    auto arr = emit_call(
        module_->getFunction(rt::str_split), {strPtr, sepPtr});
    emit_value_release(sep);
    auto iter = emit_call(module_->getFunction(rt::array_iter), {arr});
    return make_object(iter);
  }

  if (method == "starts_with" && argsAst.nodes.size() == 1) {
    auto strPtr = coerce_strlike_cstr(receiver, "sw");
    auto p = compile(*argsAst.nodes[0]);
    auto pPtr = coerce_strlike_cstr(p, "sw.pre");
    auto r = emit_call(
        module_->getFunction(rt::str_starts_with),
        {strPtr, pPtr});
    emit_value_release(p);
    return make_bool(r);
  }

  if (method == "ends_with" && argsAst.nodes.size() == 1) {
    auto strPtr = coerce_strlike_cstr(receiver, "ew");
    auto p = compile(*argsAst.nodes[0]);
    auto pPtr = coerce_strlike_cstr(p, "ew.suf");
    auto r = emit_call(
        module_->getFunction(rt::str_ends_with),
        {strPtr, pPtr});
    emit_value_release(p);
    return make_bool(r);
  }

  // --- Object methods ---

  if (method == "keys" && argsAst.nodes.size() == 0) {
    auto objPtr = expect_tag(receiver, TAG_OBJECT, "keys");
    auto arr = emit_call(
        module_->getFunction(rt::object_keys), {objPtr});
    return make_array(arr);
  }

  if (method == "has" && argsAst.nodes.size() == 1) {
    auto objPtr = expect_tag(receiver, TAG_OBJECT, "has");
    auto key = compile(*argsAst.nodes[0]);
    // Any hashable key. String tries the shape path first (so user
    // `obj.has("foo")` matches `obj.foo`); non-String / runtime
    // String land in the sidecar. The helper consumes the key's +1.
    auto r = emit_call(
        module_->getOrInsertFunction(
            rt::object_has_value, builder_.getInt1Ty(), ptrTy,
            builder_.getInt8Ty(), builder_.getInt64Ty()),
        {objPtr, extract_tag(key), extract_data(key)});
    return make_bool(r);
  }

  // Note: `.remove(x)` is routed through compile_set_mutate_dispatch
  // (called from compile_method_call before this function) so that the
  // Set and Object cases share the same runtime tag check. Same for
  // `.add(x)`.

  // --- Higher-order methods (§17.2 Array methods + §17.5 Iterator) ---
  //
  // Each higher-order method tag-dispatches on the receiver: Array
  // receivers keep the eager path (returns a new Array, same as before);
  // iterator-protocol Objects route to the iterator runtime (walks via
  // `next()`, returns Array / Long / Bool / new iterator as appropriate).

  auto ho_line = current_line_val();
  auto ho_col = current_column_val();

  // Emit Array-or-Iterator dispatch with the supplied eager/lazy body
  // emitters. Bodies produce the %Value result for their branch.
  auto dispatch_arr_iter =
      [&](const char* label,
          std::function<llvm::Value*(llvm::Value* arrPtr)> eager,
          std::function<llvm::Value*(llvm::Value* iterTag,
                                     llvm::Value* iterData)>
              lazy) -> llvm::Value* {
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto arrBB = llvm::BasicBlock::Create(
        ctx_, std::string(label) + ".arr", fn);
    auto objBB = llvm::BasicBlock::Create(
        ctx_, std::string(label) + ".obj", fn);
    auto errBB = llvm::BasicBlock::Create(
        ctx_, std::string(label) + ".err", fn);
    auto mergeBB = llvm::BasicBlock::Create(
        ctx_, std::string(label) + ".merge", fn);
    auto sw = builder_.CreateSwitch(t, errBB, 2);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrBB);
    sw->addCase(builder_.getInt8(TAG_OBJECT), objBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("Array or Object", t);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(arrBB);
    auto arrPtr = builder_.CreateIntToPtr(d, ptrTy);
    auto eagerRes = eager(arrPtr);
    auto eagerEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(objBB);
    auto lazyRes = lazy(t, d);
    auto lazyEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 2,
                                  std::string(label) + ".res");
    phi->addIncoming(eagerRes, eagerEnd);
    phi->addIncoming(lazyRes, lazyEnd);
    return phi;
  };

  if (method == "map" && argsAst.nodes.size() == 1) {
    const auto& cb_ast = *argsAst.nodes[0];
    if (is_inlinable_lambda(cb_ast, /*expected_arity=*/1)) {
      // Skip the closure construction entirely on the eager path; the
      // iterator (lazy) path still needs a closure, so we only emit
      // it inside that branch.
      return dispatch_arr_iter(
          "map",
          [&](llvm::Value* arrPtr) {
            return emit_inlined_array_map(arrPtr, cb_ast);
          },
          [&](llvm::Value* it, llvm::Value* id) {
            auto f = compile(cb_ast);
            auto out = emit_call(module_->getFunction(rt::iter_map),
                                 {it, id, extract_tag(f), extract_data(f)});
            emit_value_release(f);
            return make_object(out);
          });
    }
    auto f = compile(cb_ast);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    auto result = dispatch_arr_iter(
        "map",
        [&](llvm::Value* arrPtr) {
          auto out = emit_call(
              module_->getFunction(rt::array_map),
              {arrPtr, ft, fd, ho_line, ho_col});
          return make_array(out);
        },
        [&](llvm::Value* it, llvm::Value* id) {
          auto out = emit_call(module_->getFunction(rt::iter_map),
                               {it, id, ft, fd});
          return make_object(out);
        });
    emit_value_release(f);
    return result;
  }

  if (method == "filter" && argsAst.nodes.size() == 1) {
    const auto& cb_ast = *argsAst.nodes[0];
    if (is_inlinable_lambda(cb_ast, /*expected_arity=*/1)) {
      return dispatch_arr_iter(
          "filter",
          [&](llvm::Value* arrPtr) {
            return emit_inlined_array_filter(arrPtr, cb_ast);
          },
          [&](llvm::Value* it, llvm::Value* id) {
            auto f = compile(cb_ast);
            auto out = emit_call(module_->getFunction(rt::iter_filter),
                                 {it, id, extract_tag(f), extract_data(f)});
            emit_value_release(f);
            return make_object(out);
          });
    }
    auto f = compile(cb_ast);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    auto result = dispatch_arr_iter(
        "filter",
        [&](llvm::Value* arrPtr) {
          auto out = emit_call(
              module_->getFunction(rt::array_filter),
              {arrPtr, ft, fd, ho_line, ho_col});
          return make_array(out);
        },
        [&](llvm::Value* it, llvm::Value* id) {
          auto out = emit_call(module_->getFunction(rt::iter_filter),
                               {it, id, ft, fd});
          return make_object(out);
        });
    emit_value_release(f);
    return result;
  }

  if (method == "for_each" && argsAst.nodes.size() == 1) {
    const auto& cb_ast = *argsAst.nodes[0];
    if (is_inlinable_lambda(cb_ast, /*expected_arity=*/1)) {
      return dispatch_arr_iter(
          "foreach",
          [&](llvm::Value* arrPtr) {
            return emit_inlined_array_for_each(arrPtr, cb_ast);
          },
          [&](llvm::Value* it, llvm::Value* id) {
            llvm::Value* iter_val = llvm::UndefValue::get(valueType_);
            iter_val = builder_.CreateInsertValue(iter_val, it, {0});
            iter_val = builder_.CreateInsertValue(iter_val, id, {1});
            return emit_inlined_iter_for_each(iter_val, cb_ast);
          });
    }
    auto f = compile(cb_ast);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    auto result = dispatch_arr_iter(
        "foreach",
        [&](llvm::Value* arrPtr) {
          emit_call(module_->getFunction(rt::array_for_each),
                    {arrPtr, ft, fd, ho_line, ho_col});
          return make_nil();
        },
        [&](llvm::Value* it, llvm::Value* id) {
          emit_call(module_->getFunction(rt::iter_for_each),
                    {it, id, ft, fd, ho_line, ho_col});
          return make_nil();
        });
    emit_value_release(f);
    return result;
  }

  if (method == "reduce" && argsAst.nodes.size() == 2) {
    const auto& cb_ast = *argsAst.nodes[1];
    if (is_inlinable_lambda(cb_ast, /*expected_arity=*/2)) {
      auto init = compile(*argsAst.nodes[0]);
      return dispatch_arr_iter(
          "reduce",
          [&](llvm::Value* arrPtr) {
            return emit_inlined_array_reduce(arrPtr, init, cb_ast);
          },
          [&](llvm::Value* it, llvm::Value* id) {
            llvm::Value* iter_val = llvm::UndefValue::get(valueType_);
            iter_val = builder_.CreateInsertValue(iter_val, it, {0});
            iter_val = builder_.CreateInsertValue(iter_val, id, {1});
            return emit_inlined_iter_reduce(iter_val, init, cb_ast);
          });
    }
    auto init = compile(*argsAst.nodes[0]);
    auto f = compile(cb_ast);
    auto it_tag = extract_tag(init);
    auto it_data = extract_data(init);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto outTag =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "red.tag");
    auto outData =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "red.data");
    dispatch_arr_iter(
        "reduce",
        [&](llvm::Value* arrPtr) {
          emit_call(module_->getFunction(rt::array_reduce),
                    {arrPtr, it_tag, it_data, ft, fd, ho_line, ho_col,
                     outTag, outData});
          return make_nil();  // ignored; real result via out-params
        },
        [&](llvm::Value* it, llvm::Value* id) {
          emit_call(module_->getFunction(rt::iter_reduce),
                    {it, id, it_tag, it_data, ft, fd, ho_line, ho_col,
                     outTag, outData});
          return make_nil();
        });
    emit_value_release(f);
    auto tagLoaded = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
    auto dataLoaded = builder_.CreateLoad(builder_.getInt64Ty(), outData);
    llvm::Value* result = llvm::UndefValue::get(valueType_);
    result = builder_.CreateInsertValue(result, tagLoaded, {0});
    result = builder_.CreateInsertValue(result, dataLoaded, {1});
    return result;
  }

  if (method == "find" && argsAst.nodes.size() == 1) {
    auto f = compile(*argsAst.nodes[0]);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto outTag =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "find.tag");
    auto outData =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "find.data");
    dispatch_arr_iter(
        "find",
        [&](llvm::Value* arrPtr) {
          emit_call(module_->getFunction(rt::array_find),
                    {arrPtr, ft, fd, ho_line, ho_col, outTag, outData});
          return make_nil();
        },
        [&](llvm::Value* it, llvm::Value* id) {
          emit_call(module_->getFunction(rt::iter_find),
                    {it, id, ft, fd, ho_line, ho_col, outTag, outData});
          return make_nil();
        });
    emit_value_release(f);
    auto tagLoaded = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
    auto dataLoaded = builder_.CreateLoad(builder_.getInt64Ty(), outData);
    llvm::Value* result = llvm::UndefValue::get(valueType_);
    result = builder_.CreateInsertValue(result, tagLoaded, {0});
    result = builder_.CreateInsertValue(result, dataLoaded, {1});
    return result;
  }

  if ((method == "any" || method == "all") && argsAst.nodes.size() == 1) {
    auto f = compile(*argsAst.nodes[0]);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    auto arr_rt = method == "any" ? rt::array_any : rt::array_all;
    auto iter_rt = method == "any" ? rt::iter_any : rt::iter_all;
    auto result = dispatch_arr_iter(
        method.c_str(),
        [&](llvm::Value* arrPtr) {
          auto r = emit_call(module_->getFunction(arr_rt),
                             {arrPtr, ft, fd, ho_line, ho_col});
          return make_bool(
              builder_.CreateICmpNE(r, builder_.getInt64(0)));
        },
        [&](llvm::Value* it, llvm::Value* id) {
          auto r = emit_call(module_->getFunction(iter_rt),
                             {it, id, ft, fd, ho_line, ho_col});
          return make_bool(
              builder_.CreateICmpNE(r, builder_.getInt64(0)));
        });
    emit_value_release(f);
    return result;
  }

  if (method == "flat_map" && argsAst.nodes.size() == 1) {
    auto f = compile(*argsAst.nodes[0]);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    auto result = dispatch_arr_iter(
        "flat_map",
        [&](llvm::Value* arrPtr) {
          auto out = emit_call(
              module_->getFunction(rt::array_flat_map),
              {arrPtr, ft, fd, ho_line, ho_col});
          return make_array(out);
        },
        [&](llvm::Value* it, llvm::Value* id) {
          auto out = emit_call(module_->getFunction(rt::iter_flat_map),
                               {it, id, ft, fd, ho_line, ho_col});
          return make_object(out);
        });
    emit_value_release(f);
    return result;
  }

  // sum / product / min / max — integer aggregates, same shape for
  // Array and Iterator receivers. Non-Long elements raise a type
  // error; min/max additionally throw on empty input.
  // sum / max also accept TAG_TENSOR (the axis-less Tensor reduction
  // returns a Float); product / min stay 2-way (no Tensor support).
  if ((method == "sum" || method == "product" ||
       method == "min" || method == "max") &&
      argsAst.nodes.size() == 0) {
    const char* arr_rt_name;
    const char* iter_rt_name;
    if (method == "sum") {
      arr_rt_name = rt::array_sum;
      iter_rt_name = rt::iter_sum;
    } else if (method == "product") {
      arr_rt_name = rt::array_product;
      iter_rt_name = rt::iter_product;
    } else if (method == "min") {
      arr_rt_name = rt::array_min;
      iter_rt_name = rt::iter_min;
    } else {
      arr_rt_name = rt::array_max;
      iter_rt_name = rt::iter_max;
    }
    bool tensor_capable = (method == "sum" || method == "max");
    if (!tensor_capable) {
      return dispatch_arr_iter(
          method.c_str(),
          [&](llvm::Value* arrPtr) {
            auto n = emit_call(module_->getFunction(arr_rt_name),
                               {arrPtr, ho_line, ho_col});
            return make_long(n);
          },
          [&](llvm::Value* it, llvm::Value* id) {
            auto n = emit_call(module_->getFunction(iter_rt_name),
                               {it, id, ho_line, ho_col});
            return make_long(n);
          });
    }
    // 3-way dispatch for sum / max: TAG_ARRAY → Long, TAG_OBJECT
    // (iterator) → Long, TAG_TENSOR → Float (axis-less reduction).
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto arrBB = llvm::BasicBlock::Create(
        ctx_, std::string(method) + ".arr", fn);
    auto iterBB = llvm::BasicBlock::Create(
        ctx_, std::string(method) + ".iter", fn);
    auto tenBB = llvm::BasicBlock::Create(
        ctx_, std::string(method) + ".ten", fn);
    auto errBB = llvm::BasicBlock::Create(
        ctx_, std::string(method) + ".err", fn);
    auto mergeBB = llvm::BasicBlock::Create(
        ctx_, std::string(method) + ".merge", fn);
    auto sw = builder_.CreateSwitch(t, errBB, 3);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrBB);
    sw->addCase(builder_.getInt8(TAG_OBJECT), iterBB);
    sw->addCase(builder_.getInt8(TAG_TENSOR), tenBB);

    builder_.SetInsertPoint(arrBB);
    auto arrPtr = builder_.CreateIntToPtr(d, ptrTy);
    auto arrN = emit_call(module_->getFunction(arr_rt_name),
                          {arrPtr, ho_line, ho_col});
    auto arrV = make_long(arrN);
    auto arrEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(iterBB);
    auto iterN = emit_call(module_->getFunction(iter_rt_name),
                           {t, d, ho_line, ho_col});
    auto iterV = make_long(iterN);
    auto iterEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(tenBB);
    auto tenPtr = builder_.CreateIntToPtr(d, ptrTy);
    int op_id = (method == "sum") ? static_cast<int>(culebra::Op::Sum)
                                  : static_cast<int>(culebra::Op::Max);
    auto tenV = emit_call(
        module_->getFunction(rt::tensor_reduce_all),
        {tenPtr, builder_.getInt64(op_id)});
    auto tenEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("Array, Object, or Tensor", t);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 3,
                                  std::string(method) + ".res");
    phi->addIncoming(arrV, arrEnd);
    phi->addIncoming(iterV, iterEnd);
    phi->addIncoming(tenV, tenEnd);
    return phi;
  }

  // --- Iterator-only methods (no eager Array equivalent) ---
  // For now these error on Array receivers. Phase B2 may add eager
  // fallbacks so `arr.take(n)` becomes `arr.slice(0, n)` natively.

  // Iterator-only methods — require an iterator-protocol Object
  // receiver (Array users call `.iter()` first, matching interp).
  if (method == "collect" && argsAst.nodes.size() == 0) {
    expect_tag(receiver, TAG_OBJECT, "collect");
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out = emit_call(module_->getFunction(rt::iter_collect), {t, d});
    return make_array(out);
  }

  if (method == "count" && argsAst.nodes.size() == 0) {
    expect_tag(receiver, TAG_OBJECT, "count");
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto n = emit_call(module_->getFunction(rt::iter_count), {t, d});
    return make_long(n);
  }

  if (method == "take" && argsAst.nodes.size() == 1) {
    expect_tag(receiver, TAG_OBJECT, "take");
    auto n = value_to_long(compile(*argsAst.nodes[0]));
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out =
        emit_call(module_->getFunction(rt::iter_take), {t, d, n});
    return make_object(out);
  }

  if (method == "skip" && argsAst.nodes.size() == 1) {
    expect_tag(receiver, TAG_OBJECT, "skip");
    auto n = value_to_long(compile(*argsAst.nodes[0]));
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out =
        emit_call(module_->getFunction(rt::iter_skip), {t, d, n});
    return make_object(out);
  }

  if (method == "take_while" && argsAst.nodes.size() == 1) {
    expect_tag(receiver, TAG_OBJECT, "take_while");
    auto f = compile(*argsAst.nodes[0]);
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out = emit_call(module_->getFunction(rt::iter_take_while),
                         {t, d, extract_tag(f), extract_data(f)});
    emit_value_release(f);
    return make_object(out);
  }

  if (method == "enumerate" && argsAst.nodes.size() == 0) {
    expect_tag(receiver, TAG_OBJECT, "enumerate");
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out =
        emit_call(module_->getFunction(rt::iter_enumerate), {t, d});
    return make_object(out);
  }

  if (method == "chain" && argsAst.nodes.size() == 1) {
    expect_tag(receiver, TAG_OBJECT, "chain");
    auto other = compile(*argsAst.nodes[0]);
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out = emit_call(module_->getFunction(rt::iter_chain),
                         {t, d, extract_tag(other),
                          extract_data(other), ho_line, ho_col});
    emit_value_release(other);
    return make_object(out);
  }

  if (method == "zip" && argsAst.nodes.size() == 1) {
    expect_tag(receiver, TAG_OBJECT, "zip");
    auto other = compile(*argsAst.nodes[0]);
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out = emit_call(module_->getFunction(rt::iter_zip),
                         {t, d, extract_tag(other),
                          extract_data(other), ho_line, ho_col});
    emit_value_release(other);
    return make_object(out);
  }

  if (method == "code_points" && argsAst.nodes.size() == 0) {
    auto strPtr = coerce_strlike_cstr(receiver, "code_points");
    auto out =
        emit_call(module_->getFunction(rt::str_code_points), {strPtr});
    return make_object(out);
  }

  if (method == "graphemes" && argsAst.nodes.size() == 0) {
    auto strPtr = coerce_strlike_cstr(receiver, "graphemes");
    auto out =
        emit_call(module_->getFunction(rt::str_graphemes), {strPtr});
    return make_object(out);
  }

  if (method == "iter" && argsAst.nodes.size() == 0) {
    // Tag-dispatch on receiver: Array/Tuple/Set → array_iter on the
    // underlying members; Object → user `iter` if present, else
    // object_iter (keys); String → code_points walker.
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto arrBB = llvm::BasicBlock::Create(ctx_, "iter.arr", fn);
    auto setBB = llvm::BasicBlock::Create(ctx_, "iter.set", fn);
    auto objBB = llvm::BasicBlock::Create(ctx_, "iter.obj", fn);
    auto strBB = llvm::BasicBlock::Create(ctx_, "iter.str", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "iter.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "iter.merge", fn);
    auto sw = builder_.CreateSwitch(t, errBB, 6);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrBB);
    sw->addCase(builder_.getInt8(TAG_TUPLE), arrBB);
    sw->addCase(builder_.getInt8(TAG_SET), setBB);
    sw->addCase(builder_.getInt8(TAG_OBJECT), objBB);
    sw->addCase(builder_.getInt8(TAG_STRING), strBB);
    sw->addCase(builder_.getInt8(TAG_STRINGVIEW), strBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("Array, Tuple, Set, Object, or String", t);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(arrBB);
    auto arrPtr = builder_.CreateIntToPtr(d, ptrTy);
    auto arrIter = emit_call(module_->getFunction(rt::array_iter),
                             {arrPtr});
    auto arrVal = make_object(arrIter);
    auto arrEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(setBB);
    auto setPtr = builder_.CreateIntToPtr(d, ptrTy);
    auto setAsArr = emit_call(
        module_->getOrInsertFunction(rt::set_to_array, ptrTy, ptrTy),
        {setPtr});
    auto setIter = emit_call(module_->getFunction(rt::array_iter),
                             {setAsArr});
    auto setVal = make_object(setIter);
    auto setEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(objBB);
    // Object.iter() — defer to a user-defined `iter` slot when present
    // (mirrors interp's `_get_iterator`); a bare `{...}` literal with
    // no user iter falls back to the key iterator from
    // `ObjectValue::builtins()`.
    auto objPtr = builder_.CreateIntToPtr(d, ptrTy);
    auto objIter = emit_call(
        module_->getOrInsertFunction(
            "culebra_runtime_object_iter_dispatch", ptrTy, ptrTy),
        {objPtr});
    auto objVal = make_object(objIter);
    auto objEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(strBB);
    // String/StringView.iter() reuses code_points (Long yield) for
    // now; for-in goes through compile_for_string_loop, so this path
    // is only hit by explicit .iter() calls. Documented in §17.1.
    auto strPtr = emit_call(module_->getFunction(rt::strlike_to_cstr),
                            {t, d});
    auto strIter = emit_call(module_->getFunction(rt::str_code_points),
                             {strPtr});
    auto strVal = make_object(strIter);
    auto strEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 4, "iter.res");
    phi->addIncoming(arrVal, arrEnd);
    phi->addIncoming(setVal, setEnd);
    phi->addIncoming(objVal, objEnd);
    phi->addIncoming(strVal, strEnd);
    return phi;
  }

  if (method == "sort_by" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "sort_by");
    auto f = compile(*argsAst.nodes[0]);
    emit_call(
        module_->getFunction(rt::array_sort_by),
        {arrPtr, extract_tag(f), extract_data(f), ho_line, ho_col});
    emit_value_release(f);
    return make_nil();
  }

  return nullptr;
}

}  // namespace culebra

#endif  // CULEBRA_JIT_ENABLED
