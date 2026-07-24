#pragma once

#ifdef CULEBRA_JIT_ENABLED

// JIT value model: Shape / JitValue / JitArray / JitTensor / JitObject /
// JitCell / JitClosure / JitSet, hash & equality functors, the cycle
// collector callbacks and the backstop-GC heap glue.
//
// Runtime-layer fragment of jit.h, split out for readability. These
// fragments rely on jit.h's #include block and are included by jit.h in a
// fixed sequence (see jit.h); they are not standalone headers.

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
//
// Deliberately process-global, NOT a per-Runtime substate: a Shape is
// immutable structural metadata (names + transition tree), the same
// "shared immutable code" category as the JIT-compiled functions. The
// property inline caches embedded in that shared code cache `Shape*`
// values, so isolates running the same code on different threads MUST
// observe the same Shape pointers — a per-Runtime registry would let one
// isolate cache a Shape that dies with another isolate's heap (a real
// heap-use-after-free across worker threads). The mutable Object heap is
// what's isolated; the shape tree is shared. `transition_add` is the only
// mutator and is locked so concurrent isolates can intern safely.
struct ShapeRegistry {
  static ShapeRegistry& instance() {
    static ShapeRegistry inst;
    return inst;
  }
  Shape* root() { return root_.get(); }

  // Return the Shape obtained by adding `name` to `current`'s
  // property set. Cached on `current->add_transitions` so identical
  // transitions collide on the same Shape pointer. Locked: only ever
  // hit on inline-cache miss (rare after warm-up), so the global mutex
  // costs nothing on the steady-state fast path.
  Shape* transition_add(Shape* current, std::string_view name) {
    std::lock_guard<std::mutex> lk(mu_);
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
  std::mutex mu_;
  std::unique_ptr<Shape> root_;
  std::vector<std::unique_ptr<Shape>> owned_;  // keeps non-root shapes alive
};

inline ShapeRegistry& shape_registry() { return ShapeRegistry::instance(); }

}  // namespace culebra

// Per-Runtime slab allocator backing the hot JIT structs (see jit_slab.h).
// Lives in the kSlotJitSlab substate — placed BELOW kSlotJitGc in the slot
// enum so reverse-order Runtime teardown frees it LAST, after every
// higher-slot table dtor (module/namespace/test) has released its pinned
// JitObject/JitClosure back through operator delete. Mirrors _gc_heap();
// defined here, before the struct definitions, so each struct's operator
// new/delete body can see it.
inline culebra::SlabAllocator& _slab() {
  return culebra::runtime_substate<culebra::SlabAllocator>(
      culebra::kSlotJitSlab);
}

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
  // `tag` is int64_t (not int8_t) so the struct is two full eightbytes. A
  // `{i8, i64}` return is ABI-coerced by the C compiler to `[2 x i64]` (the i8
  // padded to an eightbyte), which does NOT match how the JIT lowers a literal
  // `{i8, i64}` return — reading the tag straight off a native call's result
  // then yields the data's low bytes. `{i64, i64}` has one unambiguous
  // two-register return ABI that the JIT and the C runtime agree on. Layout is
  // unchanged (16 bytes, tag at 0, data at 8); only the former 7 padding bytes
  // become part of the tag (always 0 — tags are small). The JIT truncates the
  // tag back to i8 on extraction, so downstream tag logic is unaffected.
  int64_t tag;
  int64_t data;
};

struct JitArray {
  int64_t refcount;
  size_t size;
  size_t capacity;
  JitValue* items;
  // Reserved unused trailing i64. Left in place to avoid perturbing the
  // established `JitArray` IR struct layout that codegen accesses by GEP index.
  int64_t gc_slot = -1;

  // Route allocation through the per-Runtime slab (jit_slab.h). Member
  // operators keep every `new JitArray()` / `delete a` call site unchanged
  // and add no field (refcount stays at offset 0). Sized delete only, so the
  // sized form is always selected and the slab knows the size class.
  static void* operator new(size_t n) { return _slab().alloc(n); }
  static void operator delete(void* p, size_t n) { _slab().free(p, n); }
};
static_assert(sizeof(JitArray) <= 48 && !std::is_polymorphic_v<JitArray>);

// Refcounted Tensor handle for the JIT runtime. `impl` is a shared_ptr
// so a graph node's `inputs` (also shared_ptr<TensorImpl>) and the JIT
// handle can co-own a TensorImpl across both interp and JIT paths.
// JIT-emitted IR only GEPs refcount/gc_slot — `impl` is touched from
// C++ runtime fns only.
struct JitTensor {
  int64_t refcount;
  int64_t gc_slot = -1;
  culebra::TensorPtr impl;

  static void* operator new(size_t n) { return _slab().alloc(n); }
  static void operator delete(void* p, size_t n) { _slab().free(p, n); }
};
static_assert(sizeof(JitTensor) <= 32 && !std::is_polymorphic_v<JitTensor>);

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

// Forward-declared; defined with the String representation below (c2
// inline length header). Referenced by value formatting / equality / puts
// above.
inline uint64_t _str_len(const char* data);
inline char* _str_alloc(uint64_t len);
inline const char* _intern_str(std::string_view s);

struct JitObject {
  int64_t refcount;
  bool has_drop = false;
  // Set once `drop` has run (explicit `obj.drop()` or the GC backstop), so
  // neither path re-runs it: drop is an at-most-once operation. Mirrors the
  // interp's OrderedSymbolMap::dropped. See _culebra_call_drop_if_present.
  bool dropped = false;
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
  // @packable handle discriminators — set once at construction. O(1)
  // type checks for the object get/set/index helpers, avoiding a
  // marker-name shape scan on every (non-packable) property/subscript.
  // Trailing fields; codegen only GEPs earlier members via offsetof.
  bool is_packed_view = false;
  bool is_shared_buffer = false;
  bool is_shared_val = false;
  bool is_fixed_array_view = false;
  // A Regex match: `m[i]` / `m["name"]` subscripts hit its capture groups
  // (mirrors interp's ObjectValue::is_match). Trailing, like the flags above.
  bool is_match = false;
  // Index into the owned-resource stack (deterministic drop, design
  // §14.3), -1 when unregistered. Set when `drop` is bound; the
  // release/sweep paths tombstone the entry through it. Trailing field —
  // codegen only GEPs earlier members.
  int64_t owned_idx = -1;
  // A builtin stdlib namespace (IO, Sys, FS, ...) — the JIT analogue of
  // interp's ObjectValue::is_namespace. Reading a member it lacks raises
  // AttributeError rather than yielding nil (see culebra_runtime_object_get_ic).
  // `ns_name` points at the namespace's interned static name for the message.
  // Trailing fields — codegen only GEPs earlier members.
  bool is_namespace = false;
  // A class object (the value bound by `class C { ... }`): `C(args)` dispatches
  // to its `new` constructor (see culebra_runtime_class_new_method), the JIT
  // twin of interp's ObjectValue::is_class. Tucked in beside is_namespace to
  // reuse its padding (JitObject stays <= 128 bytes); set via the
  // culebra_runtime_mark_class helper, never GEP'd by codegen.
  bool is_class = false;
  const char* ns_name = nullptr;

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
      auto* name_cstr = _intern_str(shape->names.back());
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

  static void* operator new(size_t n) { return _slab().alloc(n); }
  static void operator delete(void* p, size_t n) { _slab().free(p, n); }
};
static_assert(sizeof(JitObject) <= 128 && !std::is_polymorphic_v<JitObject>);

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

  static void* operator new(size_t n) { return _slab().alloc(n); }
  static void operator delete(void* p, size_t n) { _slab().free(p, n); }
};
static_assert(sizeof(JitCell) <= 32 && !std::is_polymorphic_v<JitCell>);

struct JitClosure {
  int64_t refcount;
  void* fn_ptr;
  size_t n_captures;
  JitCell** captures;
  size_t arity;  // number of user-visible params (excluding __cls__, this)
  int64_t gc_slot = -1;  // see JitArray::gc_slot

  static void* operator new(size_t n) { return _slab().alloc(n); }
  static void operator delete(void* p, size_t n) { _slab().free(p, n); }
};
static_assert(sizeof(JitClosure) <= 48 && !std::is_polymorphic_v<JitClosure>);

// Sentinel `arity` for a variadic closure (a builtin ns-method that accepts a
// range of arg counts, e.g. range/iota/Math.min). Higher-order callback
// checks (`_culebra_expect_callback`) accept it for any expected arity, since
// the trampoline validates the real count. Never collides with a real param
// count, and the well-known-prop arity==0 checks only ever see user methods.
inline constexpr size_t JIT_VARIADIC_ARITY = static_cast<size_t>(-1);

// Uniform calling convention for every JIT closure. Callers build a
// stack slab of user args and pass (cls, this, n_args, args_ptr); the
// callee extracts its declared params from `args` and bundles any
// overflow into the function-scope `__ARGS__` Array.
// The result is returned through an explicit out-pointer (`__ret`), NOT as a
// by-value 16-byte aggregate: for an INDIRECT call (through a closure fn_ptr)
// LLVM's raw-IR aggregate return disagrees with the Win64 C ABI (which returns
// a 16-byte struct via a hidden sret pointer), so a JIT→C++-thunk call would
// mis-place every argument. A plain pointer is unambiguous on every target, so
// the receiver `self` likewise crosses as two scalars (tag, data). See JitValue.
using JitFn =
    void (*)(JitValue* /*__ret*/, JitClosure*, int8_t /*this_tag*/,
             int64_t /*this_data*/, int64_t /*n_args*/, JitValue* /*args*/);

// One place that hands a receiver to a closure's fn_ptr through the JitFn ABI
// and recovers the result from the out-pointer. Callers own retain/release
// around the call as before; this wraps the ABI hand-off (out-ptr return + the
// two receiver scalars), so a future ABI tweak is a one-line change here.
inline JitValue _jit_invoke(JitClosure* fn, JitValue recv, int64_t n_args,
                            JitValue* args) {
  JitValue __ret;
  reinterpret_cast<JitFn>(fn->fn_ptr)(
      &__ret, fn, static_cast<int8_t>(recv.tag), recv.data, n_args, args);
  return __ret;
}

// Invocation helpers for the common runtime-callback arities. Each
// packs its args into a stack array and hands them to the closure's
// fn_ptr — matching what the JIT-compiled caller emits inline. Caller
// is responsible for retaining each arg before handoff; the callee
// frame takes over ownership on entry.
inline JitValue _culebra_invoke0(JitClosure* fn) {
  return _jit_invoke(fn, {0, 0} /*nil this*/, 0, nullptr);
}
inline JitValue _culebra_invoke1(JitClosure* fn, JitValue a) {
  JitValue args[1] = {a};
  return _jit_invoke(fn, {0, 0} /*nil this*/, 1, args);
}
inline JitValue _culebra_invoke2(JitClosure* fn, JitValue a, JitValue b) {
  JitValue args[2] = {a, b};
  return _jit_invoke(fn, {0, 0} /*nil this*/, 2, args);
}

// --- Cycle collector ---
//
// Python-style mark-and-sweep: tracks all refcounted heap objects, runs
// periodically and on program exit.

inline void _culebra_value_release_impl(int8_t tag, int64_t data);
inline void _culebra_cell_release(JitCell* c);
inline void _culebra_call_drop_if_present(JitObject* o);
// Raised only around `__culebra_main`'s top-level scope release at
// program exit (set via culebra_runtime_set_drop_suppressed below):
// top-level bindings leak without `drop`, matching the interpreter's
// never-torn-down global Environment. Everywhere else drop fires —
// refcount-0 cascades are precise, scope exits resolve their owned
// regions, and the GC's finalize pass backstops orphans exactly once
// (the `dropped` flag dedupes all of them). _culebra_call_drop_if_present
// honors the flag.
inline bool& _jit_drop_suppressed() {
  static thread_local bool v = false;
  return v;
}

// Toggle the suppression flag from emitted IR. `__culebra_main` wraps its
// top-level scope release at program exit in set(1)/set(0): top-level
// bindings are leaked without `drop`, matching the interpreter (whose global
// `Environment` is itself a refcount cycle and so is never torn down). The
// flag is restored to 0 afterward so a thread that goes on to run another
// isolate's `__culebra_main` keeps normal per-scope drop semantics.
extern "C" CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_set_drop_suppressed(int8_t v) {
  _jit_drop_suppressed() = (v != 0);
}

// Forward decls for the refcount helpers — `culebra_runtime_module_*`
// is emitted before the helper definitions further down (line 1430-ish)
// because the runtime header is one big TU and the module table sits
// near the throw helpers.
extern "C" {
CULEBRA_RT_KEEP void culebra_runtime_value_retain(int8_t tag, int64_t data);
CULEBRA_RT_KEEP void culebra_runtime_value_release(int8_t tag, int64_t data);
// Defined near the string helpers further down; num_add (string concat)
// needs it before that point.
CULEBRA_RT_KEEP const char* culebra_runtime_strlike_to_cstr(int8_t tag,
                                                            int64_t data);
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
// Traced-only: a heap String is registered as a GC leaf node (no children,
// no refcount) so the tracing backstop can reclaim it. It stays OUT of
// _is_refcounted_value_tag on purpose — retain/release must remain no-ops
// (zero per-op cost); only the collector reclaims strings. See jit_string.h.
static constexpr int8_t GC_TAG_STRING = TAG_STRING;
// Traced-only, like GC_TAG_STRING: the heap-allocated JitStringView descriptor
// is a GC node whose single child is the backing String it borrows (owner_base
// edge). Tracing it keeps a borrowed backing alive for exactly as long as any
// live view references it, and reclaims the descriptor itself. Also OUT of
// _is_refcounted_value_tag — retain/release stay no-ops. See jit_string.h.
static constexpr int8_t GC_TAG_STRINGVIEW = TAG_STRINGVIEW;

// Is this tag a refcounted heap value (the container/handle tags, excluding
// Cell)? Pure predicate over GC_TAG_*.
inline bool _is_refcounted_value_tag(int8_t tag) {
  return tag == GC_TAG_FUNC || tag == GC_TAG_ARRAY ||
         tag == GC_TAG_OBJECT || tag == GC_TAG_TENSOR ||
         tag == GC_TAG_TUPLE || tag == GC_TAG_SET;
}

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
inline bool _extract_bool_and_release(JitValue v);
}
extern "C" inline const char* _culebra_tag_name(int8_t tag);  // defined below

// Hash/eq for JitValue keys in JitObject's non-String key sidecar and Set.
// Key identity is Ruby's `eql?`, STRICTER than the `==` operator: keys of
// different types are never equal, so `1`, `1.0`, and `true` are three
// distinct keys even though `1 == 1.0` is true. Hash collisions across types
// are harmless (eq separates them), so the hash keeps its simple form.
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
    // Array / Set / Function / Tensor have no value-hash; mirror interp's
    // ValueHash, which throws rather than falling back to pointer identity.
    throw culebra::CulebraError(
        "TypeError",
        std::format("unhashable type: '{}'", _culebra_tag_name(v.tag)));
  }
};
struct JitValueEq {
  bool operator()(const JitValue& a, const JitValue& b) const {
    bool a_str = a.tag == TAG_STRING || a.tag == TAG_STRINGVIEW;
    bool b_str = b.tag == TAG_STRING || b.tag == TAG_STRINGVIEW;
    if (a_str || b_str) {
      return a_str && b_str &&
             _culebra_str_view(a.tag, a.data) == _culebra_str_view(b.tag, b.data);
    }
    if (a.tag != b.tag) return false;  // type-strict: no 1 == 1.0 collapse
    switch (a.tag) {
      case TAG_NIL:  return true;
      case TAG_BOOL: return a.data == b.data;
      case TAG_LONG: return a.data == b.data;
      case TAG_FLOAT: {
        double da, db;
        std::memcpy(&da, &a.data, sizeof da);
        std::memcpy(&db, &b.data, sizeof db);
        return da == db;
      }
      case TAG_TUPLE: {
        auto* aa = reinterpret_cast<JitArray*>(a.data);
        auto* bb = reinterpret_cast<JitArray*>(b.data);
        if (aa == bb) return true;
        if (aa->size != bb->size) return false;
        for (size_t i = 0; i < aa->size; i++) {
          if (!(*this)(aa->items[i], bb->items[i])) return false;
        }
        return true;
      }
      case TAG_OBJECT: {
        if (a.data == b.data) return true;
        auto* oa = reinterpret_cast<JitObject*>(a.data);
        auto* ob = reinterpret_cast<JitObject*>(b.data);
        if (auto e = _jit_object_user_eq(oa, ob)) return *e;
        return false;
      }
    }
    return false;  // unhashable tags never reach here (JitValueHash throws)
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

  static void* operator new(size_t n) { return _slab().alloc(n); }
  static void operator delete(void* p, size_t n) { _slab().free(p, n); }
};
static_assert(sizeof(JitSet) <= 48 && !std::is_polymorphic_v<JitSet>);


// Conservative backstop collector (docs/jit_gc_design.md). Lives in the
// kSlotJitGc per-Runtime slot; default-constructible for runtime_substate.
// Callbacks (defined below, after the runtime types they walk) are
// forward-declared so _gc_heap() can wire them on first use. extern "C++":
// this sits inside the surrounding extern "C" block, but the definitions
// below carry C++ linkage (they take std::vector&).
extern "C++" {
void _jit_gc_enumerate_children(void* obj, uint8_t type_tag,
                                std::vector<void*>& out);
void _jit_gc_enumerate_roots(std::vector<void*>& out);
void _jit_gc_sweep_object(void* obj, uint8_t type_tag);
}

inline void _jit_gc_finalize_dead(const std::vector<void*>& dead);

// Traced-only tags carry no refcount at offset 0 (String bytes / a view's
// borrowed ptr live there). The Heap's RC-accounting paths consult this to
// leave them out of the reference-count arithmetic. See jit_gc.h NoRcFn.
inline bool _jit_gc_is_traced_only(uint8_t type_tag) {
  return type_tag == GC_TAG_STRING || type_tag == GC_TAG_STRINGVIEW;
}

inline culebra::gc::Heap& _gc_heap() {
  auto& h = culebra::runtime_substate<culebra::gc::Heap>(culebra::kSlotJitGc);
  if (!h.callbacks_wired()) {
    h.set_children_fn(&_jit_gc_enumerate_children);
    h.set_extra_roots_fn(&_jit_gc_enumerate_roots);
    h.set_sweep_fn(&_jit_gc_sweep_object);
    h.set_finalize_fn(&_jit_gc_finalize_dead);
    h.set_no_rc_fn(&_jit_gc_is_traced_only);
    h.mark_callbacks_wired();  // arms threshold/stress collects (must be last)
  }
  return h;
}


#endif  // CULEBRA_JIT_ENABLED
