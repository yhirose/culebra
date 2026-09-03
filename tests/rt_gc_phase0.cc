// Phase 0 standalone validation for the new conservative GC heap.
// Builds nothing of the JIT — exercises include/rt_gc.h in isolation:
//   - allocation + live counters
//   - pointer validation (live / interior / random / freed)
//   - conservative stack scan finding a known root
//   - global-root registry
// Compile + run directly, e.g.:
//   clang++ -std=c++20 -O2 -Iinclude tests/rt_gc_phase0.cc -o /tmp/gc0 && /tmp/gc0

#include "rt_gc.h"

#include <cassert>
#include <cstdio>
#include <vector>

using culebra::gc::Heap;

// Run the scan on the caller's own frame so stack-held roots are in scope.
template <class Cb>
static void collect_roots(Heap& h, std::vector<void*>& out) {
  h.scan_roots([&](void* o) { out.push_back(o); });
}

static bool contains(const std::vector<void*>& v, void* p) {
  for (void* x : v) if (x == p) return true;
  return false;
}

int main() {
  Heap heap;

  // --- allocation + is_object ---
  void* a = heap.alloc(64, /*type_tag=*/1);
  void* b = heap.alloc(128, /*type_tag=*/2);
  assert(heap.live_count() == 2);
  assert(heap.live_bytes() == 192);
  assert(heap.is_object(a));
  assert(heap.is_object(b));
  assert(!heap.is_object(static_cast<char*>(a) + 8));  // interior pointer
  assert(!heap.is_object(reinterpret_cast<void*>(0x1234)));  // random
  assert(heap.header(a)->type_tag == 1);
  assert(heap.header(a)->size == 64);
  assert(heap.header(b)->generation == 0);

  // --- free → no longer a valid object ---
  void* b_key = b;  // keep the address value only as a lookup key
  heap.free_object(b);
  assert(heap.live_count() == 1);
  assert(heap.live_bytes() == 64);
  assert(!heap.is_object(b_key));

  // --- conservative stack scan finds a root held on the stack ---
  volatile void* keep = a;  // force `a` to live in stack memory
  std::vector<void*> roots;
  heap.scan_roots([&](void* o) { roots.push_back(o); });
  bool found_a = contains(roots, a);
  assert(found_a);
  (void)keep;

  // --- global-root registry: a slot whose value is a live object ---
  void* c = heap.alloc(32, /*type_tag=*/3);
  static void* c_slot;  // a "global" slot, not on the scanned test stack
  c_slot = c;
  heap.register_global_root(&c_slot);
  std::vector<void*> roots2;
  heap.scan_roots([&](void* o) { roots2.push_back(o); });
  bool found_c = contains(roots2, c);
  assert(found_c);

  std::printf(
      "jit_gc phase0: OK  (live=%zu bytes=%zu | scan found a=%d c=%d)\n",
      heap.live_count(), heap.live_bytes(), found_a, found_c);
  return 0;
}
