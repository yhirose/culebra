#pragma once
// Phase 0 of the JIT GC rewrite.
//
// Self-contained, ALLOCATE-ONLY conservative GC heap. Its job is to let
// the two genuinely dangerous parts of a conservative collector be built
// and validated *in isolation, without ever freeing*:
//   1. pointer validation  — is an arbitrary word a live object start?
//   2. conservative scanning — does the stack walk find every live root?
// Once these are trusted (unit-tested here), Phase 1 turns on mark-sweep
// based on them.
//
// Simplicity first: this is a REGISTRY heap (malloc + an address set).
// The page/bitmap allocator is a later perf phase that swaps only
// alloc/is_object/iteration — the GC algorithm (roots/mark/sweep) does
// not depend on it. No dependency on jit.h: the heap is generic over a
// raw size + a type-tag byte.

// Linux: pthread_getattr_np (used by stack_base) is a GNU extension that
// needs _GNU_SOURCE defined before <pthread.h>. g++ defines it by default
// for C++, but make it explicit so a standalone -std compile sees it too.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#ifdef _WIN32
#include <os_compat.h>  // guarded <windows.h> for GetCurrentThreadStackLimits
#else
#include <pthread.h>  // pthread_get_stackaddr_np / getattr_np (stack_base, POSIX)
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten/stack.h>  // emscripten_stack_get_{current,base}
#endif
// backtrace() for GAP5 birth-site capture. mingw and emscripten have no
// <execinfo.h>; capture degrades to "(no birth site)", which the report site
// already renders (n==0 / null symbols).
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
static inline int backtrace(void**, int) { return 0; }
static inline char** backtrace_symbols(void* const*, int) { return nullptr; }
#else
#include <execinfo.h>
#endif

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace culebra::gc {

// 8-byte per-object collector metadata. Under plan A this lives OUTSIDE the
// object, in the heap's address→metadata registry — offset 0 of a GC struct
// stays its `int64_t refcount`, so the
// existing retain/release IR is undisturbed. `type_tag` drives sweep's
// per-type destructor / enumerate_children dispatch — sweep only holds the
// raw pointer, not the JitValue tag.
struct GcHeader {
  uint8_t mark;        // 0 = unmarked (white), 1 = marked
  uint8_t type_tag;    // GC_TAG_* (opaque to the heap)
  uint8_t generation;  // 0 = young, 1 = old (Phase 2)
  uint8_t flags;       // reserved
  uint32_t size;       // total object size in bytes (header + payload)
};
static_assert(sizeof(GcHeader) == 8, "GcHeader must stay 8 bytes");

// Open-addressing address→GcHeader map: the live-object registry. Stores
// {key, header} inline in a flat array (16 bytes/slot), so registering and
// de-registering an object is a probe + slot write with NO per-entry heap
// allocation — std::unordered_map's per-object node malloc/free was the
// measured alloc-churn cost. Linear probing with
// tombstones; inserts reuse tombstone slots, so steady-state churn (insert on
// birth, erase on death) keeps tombstones bounded and rarely rehashes.
class GcRegistry {
 public:
  GcRegistry() { rehash(kInitCap); }

  // Pointer to the live entry's header, or nullptr if absent. `key` may be any
  // word from a conservative scan, so reject the empty/tombstone sentinels (no
  // real object lives at address 0 or 1) before they alias a slot's key.
  GcHeader* find(void* key) {
    if (key == kEmpty || key == kTomb()) return nullptr;
    size_t i = index(key);
    for (;;) {
      void* k = slots_[i].key;
      if (k == key) return &slots_[i].val;
      if (k == kEmpty) return nullptr;
      i = (i + 1) & mask_;
    }
  }

  // Insert (or overwrite) key→val. `key` must be a real object pointer
  // (never null or the tombstone sentinel).
  void insert(void* key, const GcHeader& val) {
    if ((used_ + 1) * 8 >= cap_ * 7) grow();
    size_t i = index(key), tomb = kNpos;
    for (;;) {
      void* k = slots_[i].key;
      if (k == key) { slots_[i].val = val; return; }
      if (k == kEmpty) break;
      if (k == kTomb() && tomb == kNpos) tomb = i;
      i = (i + 1) & mask_;
    }
    if (tomb != kNpos) i = tomb;  // reuse a tombstone (does not grow `used_`)
    else used_++;
    slots_[i].key = key;
    slots_[i].val = val;
    size_++;
  }

  void erase(void* key) {
    size_t i = index(key);
    for (;;) {
      void* k = slots_[i].key;
      if (k == key) { slots_[i].key = kTomb(); size_--; return; }
      if (k == kEmpty) return;
      i = (i + 1) & mask_;
    }
  }

  size_t size() const { return size_; }

  // Visit every live entry as f(void* key, GcHeader& val). The callback may
  // mutate the header (mark/flags) but must NOT insert/erase during the walk.
  template <class F>
  void for_each(F&& f) {
    for (auto& s : slots_)
      if (s.key != kEmpty && s.key != kTomb()) f(s.key, s.val);
  }

 private:
  struct Slot {
    void* key = kEmpty;
    GcHeader val{};
  };
  static constexpr size_t kInitCap = 16;
  static constexpr size_t kNpos = ~size_t(0);
  static constexpr void* kEmpty = nullptr;
  static void* kTomb() { return reinterpret_cast<void*>(uintptr_t(1)); }

  size_t index(void* key) const {
    uintptr_t x = reinterpret_cast<uintptr_t>(key);  // fibonacci-mix the bits
    x ^= x >> 16;
    x *= 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    return x & mask_;
  }
  void grow() {
    // Mostly tombstones → rehash same size to reclaim them; otherwise double.
    rehash(size_ * 2 >= cap_ ? cap_ * 2 : cap_);
  }
  void rehash(size_t newcap) {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(newcap, Slot{});
    cap_ = newcap;
    mask_ = newcap - 1;
    size_ = 0;
    for (auto& s : old)
      if (s.key != kEmpty && s.key != kTomb()) {
        size_t i = index(s.key);
        while (slots_[i].key != kEmpty) i = (i + 1) & mask_;
        slots_[i] = s;
        size_++;
      }
    used_ = size_;  // no tombstones in a freshly rehashed table
  }

  std::vector<Slot> slots_;
  size_t size_ = 0;  // live entries
  size_t used_ = 0;  // live + tombstones (drives probe length / rehash)
  size_t cap_ = 0;
  size_t mask_ = 0;
};

// --- wasm safepoint protocol ------------------------------------------------
//
// On wasm the conservative scan cannot reach wasm locals: they live outside
// linear memory, and setjmp spills nothing there. So an allocation site is
// never a safe place to collect — the object just allocated, and any value a
// runtime helper holds only in a local, are invisible to scan_roots. The wasm
// build therefore NEVER collects inline. A threshold trip only sets a pending
// flag, and the VM executor polls safepoint_collect() at its instruction
// boundaries (vm.h dispatch), where every live value of every frame sits in a
// register window — a linear-memory array the scan does see.
//
// One hazard remains at that poll: a runtime helper suspended BETWEEN two VM
// frames (an iterator op mid-collect invoking a user closure) may hold the
// only reference to a live object in its wasm locals. SafepointUnsafeScope
// brackets exactly those closure invocations (jit_value.h `_jit_invoke`, the
// single choke point every helper-to-user call passes through); while any
// such frame is on the stack the poll — and every explicit collect — defers.
// The dispatch call sites audited to keep all their values in registers for
// the call's whole duration use `_jit_invoke_rooted` and stay collectable.
// Marking is fail-safe by default: an unmarked new call path only delays
// collection (memory grows until the next eligible poll), never frees a live
// object.
//
// On native builds all of this folds away to the current behavior: the
// setjmp register flush plus the machine-stack walk make every frame
// scannable, and collect stays inline at the allocation site.
inline constexpr bool kDeferToSafepoint =
#ifdef __EMSCRIPTEN__
    true;
#else
    false;
#endif

// Helper frames between VM frames that may hold sole references (see above).
// Per-thread, like the Heap itself.
inline thread_local int safepoint_unsafe_depth = 0;

struct SafepointUnsafeScope {
  SafepointUnsafeScope() {
    if constexpr (kDeferToSafepoint) ++safepoint_unsafe_depth;
  }
  ~SafepointUnsafeScope() {
    if constexpr (kDeferToSafepoint) --safepoint_unsafe_depth;
  }
  SafepointUnsafeScope(const SafepointUnsafeScope&) = delete;
  SafepointUnsafeScope& operator=(const SafepointUnsafeScope&) = delete;
};

class Heap {
 public:
  // Standalone (unit-test) allocation: malloc `size` bytes and register the
  // object. The heap owns this memory and reclaims it via free_object. The
  // JIT runtime does NOT use this — it `new`s its structs (so vectors /
  // shared_ptr members are constructed) and only `adopt`s the raw pointer
  // into the registry; RC's `delete` + `forget` own that memory.
  void* alloc(uint32_t size, uint8_t type_tag) {
    void* p = raw_alloc(size);
    adopt(p, size, type_tag);
    return p;
  }

  void* raw_alloc(uint32_t size) {
    void* p = std::malloc(size);
    if (!p) std::abort();
    return p;
  }

  // Register `p` as a live object: record its collector metadata in the
  // address→metadata map (NOT in the object — offset 0 stays `refcount`).
  // The caller owns the storage (runtime: `new`; tests: raw_alloc).
  void adopt(void* p, uint32_t size, uint8_t type_tag) {
    objects_.insert(p, GcHeader{/*mark=*/0, type_tag, /*generation=*/0,
                                /*flags=*/0, size});
    live_bytes_ += size;
    if (birth_tracking()) {
      BirthSite& b = birth_sites_[p];
      b.n = backtrace(b.frames.data(), kBirthFrames);
    }
    maybe_collect();
  }

  // Once the runtime has wired the children/roots/sweep callbacks, threshold-
  // triggered collects (and GC_STRESS's collect-on-every-alloc) become live.
  bool callbacks_wired() const { return callbacks_wired_; }
  void mark_callbacks_wired() { callbacks_wired_ = true; }

  // Pin an object as a permanent root (and trace its children). Used for the
  // few program-lifetime objects built outside jit.h's reach — the cached
  // namespace objects — so the root enumerator need not cross headers.
  static constexpr uint8_t kFlagPinned = 1;
  void pin(void* p) {
    if (GcHeader* h = objects_.find(p)) h->flags |= kFlagPinned;
  }

  // Clear a pin set by pin(). Unlike the program-lifetime namespace objects,
  // some C++-held roots are ephemeral (e.g. a streaming response closure held
  // only by httplib's content provider): pin while held, unpin when dropped so
  // the object can be reclaimed once its refcount also reaches zero.
  void unpin(void* p) {
    if (GcHeader* h = objects_.find(p)) h->flags &= ~kFlagPinned;
  }

  // Pause collection across a multi-object construction whose intermediates
  // are not yet reachable from any root (e.g. building a namespace object: its
  // method closures exist registered-but-unrooted until they are slotted). A
  // collect in that window — common under GC_STRESS — would sweep them.
  void pause_collect() { ++collect_paused_; }
  void resume_collect() { --collect_paused_; }
  struct CollectPause {
    Heap& h;
    explicit CollectPause(Heap& heap) : h(heap) { h.pause_collect(); }
    ~CollectPause() { h.resume_collect(); }
  };

  // The wasm safepoint poll (vm.h dispatch, kDeferToSafepoint above). Cheap
  // pending check first; the actual collect re-arms the thresholds the same
  // way the native inline path does. Pending survives an ineligible poll
  // (paused, or a helper frame on the stack) — the next eligible one runs it.
  bool safepoint_pending() const { return collect_pending_; }
  void safepoint_collect() {
    if (collect_paused_ || safepoint_unsafe_depth > 0) return;
    collect_and_rearm();
  }

  // De-register an object whose memory the CALLER frees (RC release-to-zero
  // `delete`s the struct, then calls forget). No free/poison here — the
  // memory is not the heap's to reclaim.
  void forget(void* p) {
    if (GcHeader* h = objects_.find(p)) {
      live_bytes_ -= h->size;
      objects_.erase(p);
      if (!birth_sites_.empty()) birth_sites_.erase(p);
    }
  }

  // Free one heap-owned object (standalone alloc / Phase 1 sweep of
  // heap-owned storage). Poisons the slot so a stale pointer use is caught.
  void free_object(void* p) {
    GcHeader* h = objects_.find(p);
    if (!h) return;
    uint32_t size = h->size;
    live_bytes_ -= size;
    objects_.erase(p);
    if (!birth_sites_.empty()) birth_sites_.erase(p);
    std::memset(p, 0xDE, size);
    std::free(p);
  }

  // Conservative pointer validation: is `p` the start of a live object?
  // `data` always points to an object base (no interior pointers), so a
  // plain registry-membership test is exact for our representation.
  bool is_object(void* p) { return objects_.find(p) != nullptr; }

  GcHeader* header(void* obj) { return objects_.find(obj); }

  size_t live_count() const { return objects_.size(); }
  size_t live_bytes() const { return live_bytes_; }

  // Live count restricted to refcounted objects — traced-only values
  // (Strings/StringViews) excluded. The RC-leak fuzzer runs with the collector
  // disabled (CULEBRA_GC_NEVER) so an RC leak cannot be masked; but that also
  // stops traced-only values (whose sole reclaimer is the collector) from ever
  // being freed, so live_count() grows with benign string churn. This gives
  // that oracle an allocation measure that moves only on an actual RC leak.
  size_t rc_live_count() {
    if (!no_rc_fn_) return objects_.size();
    size_t n = 0;
    objects_.for_each([&](void*, GcHeader& h) {
      if (!no_rc_fn_(h.type_tag)) n++;
    });
    return n;
  }

  // Iterate every live object (for sweep / heap-verify).
  template <class F>
  void for_each(F&& f) {
    objects_.for_each([&](void* k, GcHeader&) { f(k); });
  }

  // Register the address of a global root slot (e.g. a namespace-table
  // pointer that lives outside any scanned stack). The scanner derefs it.
  void register_global_root(void** slot) { global_roots_.push_back(slot); }

  // Per-type child enumeration used by marking. In the real wiring this is
  // jit.h's `enumerate_children`; the heap stays decoupled by taking it as
  // a callback. `out` receives each child object pointer.
  using ChildrenFn = void (*)(void* obj, uint8_t type_tag,
                              std::vector<void*>& out);
  void set_children_fn(ChildrenFn f) { children_fn_ = f; }

  // Extra roots held in containers OUTSIDE any scanned stack — global
  // tables (module/namespace), REPL globals, the defer stack, the
  // exception carrier, multimethod bodies. The runtime registers one
  // enumerator that pushes every such live object pointer; the marker
  // calls it each collection. Missing a source here would let the
  // backstop free a still-live object, so this must be exhaustive.
  using RootFn = void (*)(std::vector<void*>& out);
  void set_extra_roots_fn(RootFn f) { extra_roots_fn_ = f; }

  // Backstop reclaimer for an unmarked object: frees the object's C++
  // buffers (items/slots/sidecars/captures) and `delete`s the struct —
  // WITHOUT firing `drop` and WITHOUT recursively releasing children
  // (they are reclaimed by their own sweep). Set by the JIT runtime; the
  // standalone heap has no struct layout so falls back to free_object.
  using SweepFn = void (*)(void* obj, uint8_t type_tag);
  void set_sweep_fn(SweepFn f) { sweep_fn_ = f; }

  // Pre-sweep finalize hook (PEP 442 style): called once per collection
  // with every unmarked object still intact, BEFORE any sweep. The JIT
  // runtime fires pending `drop`s here — the exactly-once GC backstop
  // for resources whose owner was orphaned in a cycle. The hook runs
  // user code; collection is paused around it (re-entrant collects are
  // deferred) and the runtime pins the dead set's refcounts so a drop
  // body releasing references inside it cannot free a sibling ahead of
  // its sweep.
  using FinalizeFn = void (*)(const std::vector<void*>& dead);
  void set_finalize_fn(FinalizeFn f) { finalize_fn_ = f; }

  // Predicate: does an object of this tag lack the i64 refcount slot at
  // offset 0? Traced-only values (Strings/StringViews) have content there,
  // not a refcount — reading it as one yields garbage. The RC-accounting
  // paths (compute_gc_refs and the GAP5 leak audit) use this to leave them
  // out of the reference-count arithmetic entirely; they are reclaimed by
  // the tracing mark-sweep, never by RC. Set by the JIT runtime; the
  // standalone heap (all refcounted) leaves it null (every tag has a slot).
  using NoRcFn = bool (*)(uint8_t type_tag);
  void set_no_rc_fn(NoRcFn f) { no_rc_fn_ = f; }

  // Full conservative mark-sweep. Returns objects reclaimed. MUST be
  // called directly on the mutator thread (scan walks this thread's stack
  // for roots). Non-moving.
  //
  // Soundness vs completeness: a conservative collector NEVER frees a
  // reachable object (soundness — the safety guarantee), but MAY retain an
  // unreachable one when a stale stack/register word looks like a pointer
  // to it (completeness is best-effort). So callers/tests may only assume
  // "reachable survives"; the exact reclaimed set is non-deterministic.
  size_t collect() {
    if (never()) return 0;
    // wasm: a helper frame between VM frames may hold the only reference to
    // a live object in wasm locals the scan cannot reach — defer to the next
    // eligible safepoint poll instead (kDeferToSafepoint above). This gates
    // the explicit entry points (GC.stat) the same way as threshold trips.
    if constexpr (kDeferToSafepoint) {
      if (safepoint_unsafe_depth > 0) {
        collect_pending_ = true;
        return 0;
      }
    }
    // Experimental refcount-based cycle collection (CPython gc_refs): seed
    // roots purely from the reference counts the compiler already maintains —
    // no stack scan, no shadow stack. Because a live value's refcount counts
    // it, this dissolves the safepoint-liveness problem (a freshly allocated
    // object held only in a register has refcount 1 and zero internal refs, so
    // it is a root and survives). Off by default; the brutal correctness test
    // for whether the JIT refcounts are accurate enough to retire the
    // conservative scan. See collect_refs.
    if (gc_refs_mode()) return collect_refs();
    // Experimental precise-only mode (Phase 3 dry-run): seed solely from the
    // precise roots — the JIT shadow stack plus the non-stack tables — with NO
    // conservative stack scan. A missing root frees a live object, so this is
    // the brutal coverage test for the shadow stack; off by default.
    if (precise_only()) {
      return collect_impl([this](auto&& push) { seed_nonstack_roots(push); });
    }
    // Dev oracle: measure how much of the conservative live set the precise
    // roots already cover, then collect normally (conservative stays primary).
    if (oracle()) return collect_with_oracle();
    return collect_impl([this](auto&& push) { scan_roots(push); });
  }

  // Mark from an EXPLICIT root set instead of scanning the stack. Used by
  // tests to validate mark+sweep deterministically (no conservative false
  // roots), and available to the runtime for precisely-rooted entry points.
  size_t collect_precise(const std::vector<void*>& roots) {
    return collect_impl([&roots](auto&& push) {
      for (void* r : roots) push(r);
    });
  }

  // GAP5 entry point. When CULEBRA_GC_LEAK_ABORT=1,
  // classify the heap for inflated-RC leaks and, if any survive, abort with
  // their birth sites. No-op otherwise (zero cost when off).
  //
  // MUST be called at a QUIESCENT safepoint — the mutator between top-level
  // statements or at program teardown, with the top-level env still rooted and
  // NO expression in flight. The classifier is only zero-false-positive there:
  // mid-expression, a freshly allocated object legitimately has refcount 1 and
  // zero internal refs (inflated) while its only reference sits in a register
  // the conservative scan may not have spilled yet — indistinguishable from a
  // true orphan at a single snapshot. At a quiescent point every such newborn
  // has been rooted or released, so an inflated-and-dead object is a real leak.
  // This is why it is NOT wired into the per-allocation collect() path. Runs
  // regardless of CULEBRA_GC_NEVER (the audit does not itself reclaim), so the
  // fuzzer's backstop-off mode still gets a loud teardown check.
  void maybe_audit_leaks() {
    if (leak_abort()) audit_inflated_rc();
  }

 private:
  // GC_STRESS=1 collects on every allocation (SpiderMonkey gcZeal style) so a
  // missing root or a teardown bug surfaces immediately under the test suite.
  static bool stress() {
    static const bool s = std::getenv("CULEBRA_GC_STRESS") != nullptr;
    return s;
  }
  // CULEBRA_GC_ORACLE=1 reports precise-vs-conservative coverage per collect.
  static bool oracle() {
    static const bool s = std::getenv("CULEBRA_GC_ORACLE") != nullptr;
    return s;
  }
  // CULEBRA_GC_PRECISE_ONLY=1 drops the conservative stack scan (see collect()).
  static bool precise_only() {
    static const bool s = std::getenv("CULEBRA_GC_PRECISE_ONLY") != nullptr;
    return s;
  }
  // CULEBRA_GC_REFS=1 selects refcount-based cycle collection (see collect()).
  static bool gc_refs_mode() {
    static const bool s = std::getenv("CULEBRA_GC_REFS") != nullptr;
    return s;
  }
  // CULEBRA_GC_NEVER=1 disables every collect (threshold and explicit). A
  // diagnostic discriminator for crashes: one that persists with collects off
  // cannot be a GC rooting/sweep problem — it is an RC over-release. This is
  // how the two reverted lazy-combinator "fixes" were shown to be refcount
  // bugs, not rooting gaps.
  static bool never() {
    static const bool s = std::getenv("CULEBRA_GC_NEVER") != nullptr;
    return s;
  }

  // Refcount-based cycle collection (CPython's gc_refs algorithm). Roots are
  // found from the reference counts the compiler maintains, not by scanning
  // the stack:
  //   1. gc_refs[o] = o's refcount (the i64 at offset 0 of every GC struct).
  //   2. For each object, walk its children and subtract one from each — this
  //      removes the references that live INSIDE the heap.
  //   3. Whatever refcount remains came from OUTSIDE the heap (a stack/register
  //      value, a global table, a borrow) — i.e. o is reachable from a root.
  //      Seed those (plus pinned) and let the shared core mark transitively.
  // Unmarked survivors are reachable only from each other = an unreferenced
  // cycle; the shared sweep reclaims them. RC already freed everything else.
  //
  // Safety rests on the same accuracy the deterministic-drop RC already needs:
  // enumerate_children must report each internal reference exactly as many
  // times as it is counted in the child's refcount (no over-report). An
  // UNDER-report or a too-high refcount only over-retains (safe); the danger
  // is over-subtraction, which the GC_STRESS + difftest gates catch. This is
  // the same algorithm the interpreter's InterpGC uses, so the two backends
  // share one cycle-collection model.
  // Build the gc_refs map: each object's refcount (the i64 @ offset 0) minus
  // its internal (heap) in-edges. A positive residue means a reference from
  // OUTSIDE the heap (a root/borrow) still holds it. This edge accounting is
  // load-bearing and over-subtraction is the one unsafe direction (see the
  // collect_refs docblock), so both users — collect_refs (cycle collection) and
  // classify_leaks (leak diagnostics) — share this single construction.
  void compute_gc_refs(std::unordered_map<void*, int64_t>& gc_refs) {
    gc_refs.reserve(objects_.size());
    objects_.for_each([&](void* o, GcHeader& h) {
      // Traced-only tags have no refcount at offset 0 (content lives there),
      // so they take no part in RC arithmetic — the tracing mark-sweep is
      // their sole reclaimer. Leaving them out of the map means they are
      // never seeded as RC roots and, as children, never decrement anyone
      // (find() misses). A String reached only from a live container is
      // still marked via children_fn during the reachability walk.
      if (no_rc_fn_ && no_rc_fn_(h.type_tag)) return;
      gc_refs.emplace(o, *reinterpret_cast<int64_t*>(o));  // refcount @ off 0
    });
    std::vector<void*> kids;
    objects_.for_each([&](void* o, GcHeader& h) {
      if (!children_fn_) return;
      kids.clear();
      children_fn_(o, h.type_tag, kids);
      for (void* c : kids) {
        auto it = gc_refs.find(c);
        if (it != gc_refs.end()) it->second--;
      }
    });
  }

  size_t collect_refs() {
    if (gc_refs_diag()) collect_refs_diag();
    return collect_impl([this](auto&& push) {
      std::unordered_map<void*, int64_t> gc_refs;
      compute_gc_refs(gc_refs);
      for (auto& [o, refs] : gc_refs) {
        if (refs > 0) push(o);
      }
      // Traced-only values (Strings/Views) carry no refcount to seed from and
      // can live purely on the stack, so the refcount seed above cannot root
      // them — freeing one still borrowed would crash. Seed them the way the
      // conservative collector does: a stack scan restricted to their tags.
      // Dead ones are still swept (the scan won't find them), so refs-mode
      // string reclamation matches conservative — leaving this experiment a
      // faithful RC-correctness test for the refcounted objects it targets.
      if (no_rc_fn_)
        scan_roots([&](void* o) {
          GcHeader* h = objects_.find(o);
          if (h && no_rc_fn_(h->type_tag)) push(o);
        });
    });
  }

  static bool gc_refs_diag() {
    static const bool s = std::getenv("CULEBRA_GC_REFS_DIAG") != nullptr;
    return s;
  }

  // GAP5 — loud leak detection. With
  // CULEBRA_GC_LEAK_ABORT=1, the teardown quiescent-point audit (see
  // maybe_audit_leaks) classifies the conservative-dead-but-refcount-reachable
  // objects; any *inflated-RC* one (a phantom +1 = a definite acyclic RC leak,
  // distinct from a benign cycle) is reported with its allocation backtrace and
  // the process aborts. This turns the backstop's silent masking into an
  // actionable crash at the birth site. Off by default: production keeps
  // reclaiming (a crash would be a DoS trade, §5-GAP5); this is the debug/CI
  // net. Enabling it auto-enables birth-site capture (see birth_tracking).
  static bool leak_abort() {
    static const bool s = std::getenv("CULEBRA_GC_LEAK_ABORT") != nullptr;
    return s;
  }

  // Record each object's allocation backtrace so leak_abort can name the birth
  // site. Costly (a backtrace() per adopt), so it is opt-in: on automatically
  // under leak_abort, or standalone via CULEBRA_GC_BIRTH_SITE for diagnostics.
  static bool birth_tracking() {
    static const bool s =
        leak_abort() || std::getenv("CULEBRA_GC_BIRTH_SITE") != nullptr;
    return s;
  }

  // Classify why gc_refs over-retains: mark the gc_refs-reachable set and the
  // conservative-reachable set; every object gc_refs keeps that the
  // conservative scan would free is a leak. Split them: an *inflated-RC* object
  // (refcount exceeds its counted internal references) carries a phantom +1 =
  // a definite acyclic RC leak; a *transitively-held* one (refcount == internal
  // count, gc_refs residue 0) is kept only via a retained-live ancestor — the
  // shape a benign reference cycle takes, not an RC bug. Fills `inflated` with
  // the former and `by_tag` with the per-type leak histogram; returns the total
  // leaked count. This is the zero-false-positive detector GAP5 makes loud.
  size_t classify_leaks(std::vector<void*>& inflated, size_t by_tag[256],
                        int64_t& transitively_held) {
    std::unordered_map<void*, int64_t> gc_refs;
    compute_gc_refs(gc_refs);
    std::unordered_set<void*> refs_live, cons_live;
    mark_reachable(refs_live, [&](auto&& push) {
      for (auto& [o, r] : gc_refs) if (r > 0) push(o);
    });
    mark_reachable(cons_live, [this](auto&& push) { scan_roots(push); });
    size_t leak = 0;
    transitively_held = 0;
    for (void* o : refs_live) {
      if (cons_live.count(o)) continue;  // genuinely live — keep
      leak++;
      GcHeader* h = objects_.find(o);
      if (h) by_tag[h->type_tag]++;
      // gc_refs[o] here is refcount - internal; > 0 means external refs remain.
      if (gc_refs[o] > 0) inflated.push_back(o);  // phantom +1 = RC leak
      else transitively_held++;                   // via a retained-live ancestor
    }
    return leak;
  }

  // Print the leak classification (CULEBRA_GC_REFS_DIAG diagnostic).
  void collect_refs_diag() {
    std::vector<void*> inflated;
    size_t by_tag[256] = {0};
    int64_t transitively_held = 0;
    size_t leak = classify_leaks(inflated, by_tag, transitively_held);
    std::fprintf(stderr,
        "[gc-refs-diag] heap=%zu leak=%lld "
        "(inflated_rc=%zu transitively_held=%lld) tags:",
        objects_.size(), (long long)leak, inflated.size(),
        (long long)transitively_held);
    for (int t = 0; t < 256; t++)
      if (by_tag[t]) std::fprintf(stderr, " %d=%zu", t, by_tag[t]);
    std::fprintf(stderr, "\n");
  }

  // GAP5 worker (call only via maybe_audit_leaks, at a quiescent safepoint).
  // Classify leaks; if any inflated-RC object survives — a definite acyclic RC
  // leak the backstop would otherwise silently reclaim — dump each one's type
  // and allocation birth site, then abort so the bug surfaces where it was
  // born. Benign cycles never trigger this (their residue is transitively-held,
  // not inflated).
  void audit_inflated_rc() {
    std::vector<void*> inflated;
    size_t by_tag[256] = {0};
    int64_t transitively_held = 0;
    classify_leaks(inflated, by_tag, transitively_held);
    if (inflated.empty()) return;
    std::fprintf(stderr,
        "\n[gc-leak-abort] %zu inflated-RC object(s) detected — a phantom +1 "
        "the backstop would silently reclaim. This is a definite RC leak.\n",
        inflated.size());
    size_t shown = 0;
    for (void* o : inflated) {
      if (shown++ >= kMaxLeakReports) {
        std::fprintf(stderr, "  ... and %zu more (capped)\n",
                     inflated.size() - kMaxLeakReports);
        break;
      }
      GcHeader* h = objects_.find(o);
      std::fprintf(stderr, "\n  leak: obj=%p tag=%d refcount=%lld\n", o,
                   h ? h->type_tag : -1,
                   (long long)*reinterpret_cast<int64_t*>(o));
      auto it = birth_sites_.find(o);
      if (it == birth_sites_.end() || it->second.n == 0) {
        std::fprintf(stderr,
                     "    (no birth site — set CULEBRA_GC_BIRTH_SITE=1)\n");
        continue;
      }
      char** syms = backtrace_symbols(it->second.frames.data(), it->second.n);
      std::fprintf(stderr, "    born at:\n");
      for (int i = 0; i < it->second.n; i++)
        std::fprintf(stderr, "      %s\n", syms ? syms[i] : "?");
      std::free(syms);
    }
    std::fflush(stderr);
    std::abort();
  }

  // The non-stack root sources: registered global slots plus the runtime's
  // extra-roots enumerator (module/namespace tables, defer stack, exception
  // carrier, and the precise JIT shadow stack). This is the conservative
  // scan minus the machine-stack walk — the seed for precise collection.
  template <class Cb>
  void seed_nonstack_roots(Cb&& cb) {
    for (void** slot : global_roots_) {
      if (slot && is_object(*slot)) cb(*slot);
    }
    if (extra_roots_fn_) {
      extra_roots_.clear();
      extra_roots_fn_(extra_roots_);
      for (void* o : extra_roots_) {
        if (is_object(o)) cb(o);
      }
    }
  }

  // Mark the reachable set from a seed into `live` (no sweep). Shared by the
  // oracle's two passes.
  template <class Seed>
  void mark_reachable(std::unordered_set<void*>& live, Seed&& seed) {
    std::vector<void*> work, kids;
    auto push = [&](void* o) {
      if (objects_.find(o) && live.insert(o).second) work.push_back(o);
    };
    objects_.for_each([&](void* k, GcHeader& h) {
      if (h.flags & kFlagPinned) push(k);
    });
    seed(push);
    while (!work.empty()) {
      void* o = work.back();
      work.pop_back();
      if (children_fn_) {
        kids.clear();
        children_fn_(o, objects_.find(o)->type_tag, kids);
        for (void* c : kids) push(c);
      }
    }
  }

  // Dev oracle: compute the conservative and precise reachable sets on the
  // same pre-sweep population, report the gap, then collect conservatively.
  // The "missed" count (reachable conservatively but not precisely) is an
  // upper bound on the real coverage gap: it also includes conservative
  // false positives (dead objects a stale stack word retains), so it
  // overstates rather than understates. Trending toward a small, stable
  // floor across development means the shadow stack covers the live set.
  size_t collect_with_oracle() {
    std::unordered_set<void*> precise, conserv;
    mark_reachable(precise, [this](auto&& push) { seed_nonstack_roots(push); });
    mark_reachable(conserv, [this](auto&& push) { scan_roots(push); });
    size_t missed = 0;
    for (void* o : conserv) {
      if (!precise.count(o)) missed++;
    }
    double covered = conserv.empty()
        ? 100.0
        : 100.0 * static_cast<double>(conserv.size() - missed) /
              static_cast<double>(conserv.size());
    std::fprintf(stderr,
        "[gc-oracle] heap=%zu conservative=%zu precise=%zu missed=%zu "
        "(%.1f%% precise-covered)\n",
        objects_.size(), conserv.size(), precise.size(), missed, covered);
    return collect_impl([this](auto&& push) { scan_roots(push); });
  }

  // Threshold-triggered collect, run from adopt(). Only fires once the runtime
  // has wired its callbacks (the standalone heap collects explicitly), and
  // adapts the next threshold to the post-collect live set to keep the
  // amortised cost ~constant while bounding peak RSS.
  static constexpr size_t kMinThreshold = 100000;
  static constexpr size_t kByteFloor = 16u * 1024 * 1024;
  void maybe_collect() {
    if (never() || !callbacks_wired_ || collect_paused_) return;
    ++alloc_since_collect_;
    // Two triggers, whichever fires first:
    //  - count-based (objects*16 amortization): tuned for RC-reclaimed heaps
    //    where the collector is only a reference-CYCLE backstop.
    //  - bytes-based (classic tracing-GC growth factor): required for
    //    traced-only values (Strings) that RC never frees, so the collector
    //    is their sole reclaimer. Bounds inter-collect resident growth to ~2x
    //    the post-collect live set (kByteFloor floor). It rarely fires for
    //    RC'd workloads — their live_bytes_ stays bounded via RC — so the
    //    microgpt/autograd amortization is left untouched.
    if (alloc_since_collect_ < collect_threshold_ &&
        live_bytes_ < byte_threshold_)
      return;
    // wasm: never collect at the allocation site — the object just adopted
    // (and anything a helper holds only in wasm locals) is invisible to the
    // scan. Flag it; the VM's safepoint poll runs the collect.
    if constexpr (kDeferToSafepoint) {
      collect_pending_ = true;
      return;
    }
    collect_and_rearm();
  }

  // Run a collect and reset/re-arm the trigger state as one unit — shared by
  // the native inline path above and the wasm safepoint path, so a future
  // collect entry point cannot forget the re-arm half.
  void collect_and_rearm() {
    collect_pending_ = false;
    alloc_since_collect_ = 0;
    collect();
    rearm_thresholds();
  }

  void rearm_thresholds() {
    // The backstop is only the reclaimer of reference CYCLES — RC frees
    // everything acyclic immediately and deterministically (now leak-free on
    // the normal path), so the whole-heap subtraction pass can run far less
    // often than the per-object allocation rate. Cycles accumulate slowly,
    // so amortise it over ~16× the live set:
    // a scalar-autograd profile spends ~19% in GC bookkeeping, and raising the
    // threshold from 2 to 16 recovers ~11% wall-clock on microgpt with the
    // heap staying bounded (RC reclaims the acyclic residue regardless). The
    // multiplier is tunable via CULEBRA_GC_MULT for the frequency/memory study.
    static const size_t mult = []() -> size_t {
      if (const char* m = std::getenv("CULEBRA_GC_MULT")) {
        int64_t v = std::atol(m);
        if (v > 0) return static_cast<size_t>(v);
      }
      return 16;
    }();
    collect_threshold_ =
        stress() ? 1 : std::max(kMinThreshold, objects_.size() * mult);
    // Re-arm the bytes trigger to ~2x the post-collect live set (classic
    // tracing-GC growth factor), never below the floor.
    byte_threshold_ = std::max(kByteFloor, live_bytes_ * 2);
  }

  // Shared mark-sweep core. `seed(push)` supplies the initial roots.
  template <class Seed>
  size_t collect_impl(Seed&& seed) {
    if (collect_paused_) return 0;  // defer re-entrant/paused collects
    objects_.for_each([](void*, GcHeader& h) { h.mark = 0; });

    std::vector<void*> work;
    auto push = [&](void* o) {
      GcHeader* h = objects_.find(o);
      if (h && !h->mark) {
        h->mark = 1;
        work.push_back(o);
      }
    };
    // Pinned objects are always roots.
    objects_.for_each([&](void* k, GcHeader& h) {
      if (h.flags & kFlagPinned && !h.mark) {
        h.mark = 1;
        work.push_back(k);
      }
    });
    seed(push);

    std::vector<void*> kids;
    while (!work.empty()) {
      void* o = work.back();
      work.pop_back();
      if (children_fn_) {
        kids.clear();
        children_fn_(o, objects_.find(o)->type_tag, kids);
        for (void* c : kids) push(c);
      }
    }

    // Sweep unmarked. With a runtime sweep_fn the object is a `new`d JIT
    // struct: de-register it (before delete, so a re-entrant lookup can't
    // see it), then run the buffer-teardown + delete. Without one (the
    // standalone heap) fall back to free_object's malloc/free + poison.
    std::vector<void*> dead;
    objects_.for_each([&](void* k, GcHeader& h) {
      if (!h.mark) dead.push_back(k);
    });
    // Finalize before any sweep: every dead object is still intact, so a
    // drop body can safely touch its (equally dead) neighbours. The hook
    // runs user code — pause threshold collects, and note that new
    // objects it allocates are not in `dead` and so survive this sweep.
    if (finalize_fn_ && !dead.empty()) {
      CollectPause pause(*this);
      finalize_fn_(dead);
    }
    for (void* p : dead) {
      if (sweep_fn_) {
        GcHeader* h = objects_.find(p);
        uint8_t tag = h->type_tag;
        live_bytes_ -= h->size;
        objects_.erase(p);
        sweep_fn_(p, tag);
      } else {
        free_object(p);
      }
    }
    return dead.size();
  }

 public:

  // Conservatively scan the current thread's machine stack + flushed
  // registers + registered globals; call `cb(obj)` for each candidate
  // root found. MUST be called directly on the mutator thread whose stack
  // holds the live values (no helper frame in between drops them).
  template <class Cb>
  void scan_roots(Cb&& cb) {
    // Flush callee-saved registers onto the stack (the Boehm/Ruby trick):
    // setjmp writes them into `jb`, which lives in this frame, so the
    // stack walk below covers them.
    jmp_buf jb;
    (void)setjmp(jb);
    // Low end of the live stack region = the current stack pointer. NOT
    // __builtin_frame_address(0): that returns the frame *pointer*, but
    // locals and register spills sit BELOW it, so scanning from FP misses
    // the current frame (this bit at -O2 in Phase 0). Read SP directly.
    void* sp = current_sp();
    void* base = stack_base();
    scan_range(sp, base, cb);
    // Conservative = the machine-stack walk above PLUS the non-stack roots.
    // Sharing seed_nonstack_roots keeps the conservative and precise seeds
    // from drifting apart as root sources are added.
    seed_nonstack_roots(cb);
  }

 private:
  // Scan [lo, hi) word-aligned, reporting any word that is a live object.
  //
  // The conservative read of every stack word is deliberately out of
  // bounds w.r.t. any single object: it walks across frames, padding, and
  // ASan's poisoned redzones. Exempt the function from AddressSanitizer
  // (the standard Boehm/Ruby trick) so that intentional scan doesn't trip
  // a stack-buffer-underflow; without it ASan aborts every JIT GC collect.
  template <class Cb>
  __attribute__((no_sanitize("address")))
  void scan_range(void* lo, void* hi, Cb&& cb) {
    auto a = reinterpret_cast<uintptr_t>(lo);
    auto b = reinterpret_cast<uintptr_t>(hi);
    if (a > b) std::swap(a, b);
    a = (a + sizeof(void*) - 1) & ~(uintptr_t)(sizeof(void*) - 1);  // align up
    for (auto w = a; w + sizeof(void*) <= b; w += sizeof(void*)) {
      void* candidate = *reinterpret_cast<void**>(w);
      if (is_object(candidate)) cb(candidate);
    }
  }

  // Current stack pointer (low end of the live region). The frame
  // pointer is too high — it excludes this frame's locals/spills.
  static void* current_sp() {
#if defined(__aarch64__) || defined(__arm64__)
    void* sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    return sp;
#elif defined(__x86_64__)
    void* sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    return sp;
#elif defined(__EMSCRIPTEN__)
    // wasm keeps its stack pointer in a global, not a register: ask the
    // runtime for it. Taking a local's address instead returns a pointer
    // into this frame, which is dead by the time the caller reads it.
    return reinterpret_cast<void*>(emscripten_stack_get_current());
#else
#error "conservative GC current_sp: unsupported platform"
#endif
  }

  // Highest address of the current thread's stack (it grows down toward
  // lower addresses, so the live region is [current_sp, base)). The query
  // is the conservative scanner's one OS-specific point.
  static void* stack_base() {
#if defined(__APPLE__)
    return pthread_get_stackaddr_np(pthread_self());
#elif defined(__linux__)
    // glibc: pthread_attr_getstack returns the LOW address + size; the base
    // (highest address, where the stack starts) is addr + size.
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) std::abort();
    void* addr = nullptr;
    size_t size = 0;
    pthread_attr_getstack(&attr, &addr, &size);
    pthread_attr_destroy(&attr);
    return static_cast<char*>(addr) + size;
#elif defined(_WIN32)
    // The TIB records the current thread's stack bounds; the high limit is the
    // base (the stack grows down toward the low limit). GetCurrentThreadStackLimits
    // is available on Windows 8 / Server 2012+.
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    return reinterpret_cast<void*>(high);
#elif defined(__EMSCRIPTEN__)
    // wasm's value stack lives outside linear memory, where nothing can scan
    // it; the user stack emscripten reserves inside it is where an
    // address-taken local goes — the only locals a conservative scan finds.
    return reinterpret_cast<void*>(emscripten_stack_get_base());
#else
#error "conservative GC stack_base: unsupported platform"
#endif
  }

  // GAP5 birth-site side table (populated only under birth_tracking): the
  // captured allocation backtrace per live object, symbolized on a leak abort.
  // A side map keeps GcHeader at its load-bearing 8 bytes and costs nothing
  // when the feature is off.
  static constexpr int kBirthFrames = 12;
  static constexpr size_t kMaxLeakReports = 20;  // cap the abort dump
  struct BirthSite {
    std::array<void*, kBirthFrames> frames{};
    int n = 0;
  };
  std::unordered_map<void*, BirthSite> birth_sites_;

  GcRegistry objects_;
  std::vector<void**> global_roots_;
  std::vector<void*> extra_roots_;  // scratch reused across collections
  ChildrenFn children_fn_ = nullptr;
  RootFn extra_roots_fn_ = nullptr;
  SweepFn sweep_fn_ = nullptr;
  FinalizeFn finalize_fn_ = nullptr;
  NoRcFn no_rc_fn_ = nullptr;
  size_t live_bytes_ = 0;
  bool callbacks_wired_ = false;
  int collect_paused_ = 0;
  bool collect_pending_ = false;  // wasm safepoint flag (kDeferToSafepoint)
  size_t alloc_since_collect_ = 0;
  size_t collect_threshold_ = stress() ? 1 : kMinThreshold;
  size_t byte_threshold_ = kByteFloor;
};

}  // namespace culebra::gc
