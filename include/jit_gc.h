#pragma once
// Phase 0 of the JIT GC rewrite — see docs/jit_gc_design.md.
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
#include <pthread.h>

#include <algorithm>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace culebra::gc {

// 8-byte per-object collector metadata. Under plan A (docs/jit_gc_design.md
// §2 revision) this lives OUTSIDE the object, in the heap's address→metadata
// registry — offset 0 of a GC struct stays its `int64_t refcount`, so the
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
// measured alloc-churn cost (docs/jit_gc_design.md §4). Linear probing with
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

  // De-register an object whose memory the CALLER frees (RC release-to-zero
  // `delete`s the struct, then calls forget). No free/poison here — the
  // memory is not the heap's to reclaim.
  void forget(void* p) {
    if (GcHeader* h = objects_.find(p)) {
      live_bytes_ -= h->size;
      objects_.erase(p);
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

 private:
  // GC_STRESS=1 collects on every allocation (SpiderMonkey gcZeal style) so a
  // missing root or a teardown bug surfaces immediately under the test suite.
  static bool stress() {
    static const bool s = std::getenv("CULEBRA_GC_STRESS") != nullptr;
    return s;
  }

  // Threshold-triggered collect, run from adopt(). Only fires once the runtime
  // has wired its callbacks (the standalone heap collects explicitly), and
  // adapts the next threshold to the post-collect live set to keep the
  // amortised cost ~constant while bounding peak RSS.
  static constexpr size_t kMinThreshold = 100000;
  void maybe_collect() {
    if (!callbacks_wired_) return;
    if (++alloc_since_collect_ < collect_threshold_) return;
    alloc_since_collect_ = 0;
    collect();
    collect_threshold_ =
        stress() ? 1 : std::max(kMinThreshold, objects_.size() * 2);
  }

  // Shared mark-sweep core. `seed(push)` supplies the initial roots.
  template <class Seed>
  size_t collect_impl(Seed&& seed) {
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

 private:
  // Scan [lo, hi) word-aligned, reporting any word that is a live object.
  template <class Cb>
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
#else
    void* marker;
    return &marker;  // address of a local ≈ SP (portable fallback)
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
#else
#error "conservative GC stack_base: unsupported platform"
#endif
  }

  GcRegistry objects_;
  std::vector<void**> global_roots_;
  std::vector<void*> extra_roots_;  // scratch reused across collections
  ChildrenFn children_fn_ = nullptr;
  RootFn extra_roots_fn_ = nullptr;
  SweepFn sweep_fn_ = nullptr;
  size_t live_bytes_ = 0;
  bool callbacks_wired_ = false;
  size_t alloc_since_collect_ = 0;
  size_t collect_threshold_ = stress() ? 1 : kMinThreshold;
};

}  // namespace culebra::gc
