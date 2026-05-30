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

#include <pthread.h>

#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace culebra::gc {

// 8-byte object header at offset 0 of every GC-managed struct (replacing
// the old `int64_t refcount`). `type_tag` drives sweep's per-type
// destructor / enumerate_children dispatch — sweep only holds the raw
// pointer, not the JitValue tag.
struct GcHeader {
  uint8_t mark;        // 0 = unmarked (white), 1 = marked
  uint8_t type_tag;    // GC_TAG_* (opaque to the heap)
  uint8_t generation;  // 0 = young, 1 = old (Phase 2)
  uint8_t flags;       // reserved
  uint32_t size;       // total object size in bytes (header + payload)
};
static_assert(sizeof(GcHeader) == 8, "GcHeader must stay 8 bytes");

class Heap {
 public:
  // Allocate `size` bytes (header included) and register the object.
  // Returns the object pointer (GcHeader at offset 0). Allocate-only:
  // never frees or collects in Phase 0.
  void* alloc(uint32_t size, uint8_t type_tag) {
    void* p = raw_alloc(size);
    adopt(p, size, type_tag);
    return p;
  }

  // Two-step allocation for callers that must run a C++ constructor on the
  // payload (vectors, shared_ptr members): `raw_alloc` hands back unregistered
  // storage, the caller placement-news the struct (value-initialising — and
  // thus zeroing — the leading GcHeader), then `adopt` stamps the real header
  // and registers the object. Splitting the two is what keeps the header
  // intact: stamping before the placement-new would just be clobbered by the
  // constructor. `alloc` above is the fused form for callers that don't need
  // to construct (the Phase 0 tests). `adopt` is the single source of truth
  // for header layout + registration.
  void* raw_alloc(uint32_t size) {
    void* p = std::malloc(size);
    if (!p) std::abort();
    return p;
  }
  void adopt(void* p, uint32_t size, uint8_t type_tag) {
    auto* h = static_cast<GcHeader*>(p);
    h->mark = 0;
    h->type_tag = type_tag;
    h->generation = 0;
    h->flags = 0;
    h->size = size;
    objects_.insert(p);
    live_bytes_ += size;
  }

  // Free one object. Phase 1 sweep calls this after running the object's
  // C++ destructor; in Phase 0 it exists only for the unit tests. Poisons
  // the slot so a stale pointer use is caught.
  void free_object(void* p) {
    auto it = objects_.find(p);
    if (it == objects_.end()) return;
    uint32_t size = static_cast<GcHeader*>(p)->size;
    live_bytes_ -= size;
    objects_.erase(it);
    std::memset(p, 0xDE, size);
    std::free(p);
  }

  // Conservative pointer validation: is `p` the start of a live object?
  // `data` always points to an object base (no interior pointers), so a
  // plain registry-membership test is exact for our representation.
  bool is_object(const void* p) const {
    return objects_.find(const_cast<void*>(p)) != objects_.end();
  }

  GcHeader* header(void* obj) const { return static_cast<GcHeader*>(obj); }

  size_t live_count() const { return objects_.size(); }
  size_t live_bytes() const { return live_bytes_; }

  // Iterate every live object (for sweep / heap-verify).
  template <class F>
  void for_each(F&& f) const {
    for (void* p : objects_) f(p);
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
  // Shared mark-sweep core. `seed(push)` supplies the initial roots.
  template <class Seed>
  size_t collect_impl(Seed&& seed) {
    for_each([](void* p) { static_cast<GcHeader*>(p)->mark = 0; });

    std::vector<void*> work;
    auto push = [&](void* o) {
      if (!is_object(o)) return;
      auto* h = static_cast<GcHeader*>(o);
      if (!h->mark) {
        h->mark = 1;
        work.push_back(o);
      }
    };
    seed(push);

    std::vector<void*> kids;
    while (!work.empty()) {
      void* o = work.back();
      work.pop_back();
      if (children_fn_) {
        kids.clear();
        children_fn_(o, static_cast<GcHeader*>(o)->type_tag, kids);
        for (void* c : kids) push(c);
      }
    }

    // Sweep unmarked. Real wiring runs each object's C++ destructor here
    // (RAII frees items/slots/sidecars); the isolated heap just frees.
    std::vector<void*> dead;
    for_each([&](void* p) {
      if (!static_cast<GcHeader*>(p)->mark) dead.push_back(p);
    });
    for (void* p : dead) free_object(p);
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
  // lower addresses, so the live region is [current_sp, base)).
  static void* stack_base() {
    return pthread_get_stackaddr_np(pthread_self());
  }

  std::unordered_set<void*> objects_;
  std::vector<void**> global_roots_;
  ChildrenFn children_fn_ = nullptr;
  size_t live_bytes_ = 0;
};

}  // namespace culebra::gc
