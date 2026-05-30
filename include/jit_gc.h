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
#include <unordered_map>
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
    // `p` is a fresh address at every object birth, so emplace (construct the
    // metadata in place, no default-construct + assign) is both correct and
    // the cheaper insert on this per-allocation path.
    objects_.emplace(p, GcHeader{/*mark=*/0, type_tag, /*generation=*/0,
                                 /*flags=*/0, size});
    live_bytes_ += size;
  }

  // De-register an object whose memory the CALLER frees (RC release-to-zero
  // `delete`s the struct, then calls forget). No free/poison here — the
  // memory is not the heap's to reclaim.
  void forget(void* p) {
    auto it = objects_.find(p);
    if (it == objects_.end()) return;
    live_bytes_ -= it->second.size;
    objects_.erase(it);
  }

  // Free one heap-owned object (standalone alloc / Phase 1 sweep of
  // heap-owned storage). Poisons the slot so a stale pointer use is caught.
  void free_object(void* p) {
    auto it = objects_.find(p);
    if (it == objects_.end()) return;
    uint32_t size = it->second.size;
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

  GcHeader* header(void* obj) {
    auto it = objects_.find(obj);
    return it == objects_.end() ? nullptr : &it->second;
  }

  size_t live_count() const { return objects_.size(); }
  size_t live_bytes() const { return live_bytes_; }

  // Iterate every live object (for sweep / heap-verify).
  template <class F>
  void for_each(F&& f) const {
    for (const auto& kv : objects_) f(kv.first);
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
    for (auto& kv : objects_) kv.second.mark = 0;

    std::vector<void*> work;
    auto push = [&](void* o) {
      auto it = objects_.find(o);
      if (it == objects_.end()) return;
      if (!it->second.mark) {
        it->second.mark = 1;
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
        children_fn_(o, objects_.at(o).type_tag, kids);
        for (void* c : kids) push(c);
      }
    }

    // Sweep unmarked. Real wiring runs each object's C++ destructor here
    // (RAII frees items/slots/sidecars); the isolated heap just frees.
    std::vector<void*> dead;
    for (const auto& kv : objects_) {
      if (!kv.second.mark) dead.push_back(kv.first);
    }
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

  std::unordered_map<void*, GcHeader> objects_;
  std::vector<void**> global_roots_;
  ChildrenFn children_fn_ = nullptr;
  size_t live_bytes_ = 0;
};

}  // namespace culebra::gc
