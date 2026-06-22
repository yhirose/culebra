#pragma once

// Per-Runtime non-moving, size-segregated slab allocator for the hot JIT
// runtime structs (JitObject / JitArray / JitCell / JitClosure / ...) and
// their variable-length buffers. It collapses per-object malloc/free/madvise
// syscalls into amortized 64KB chunk carving: the autograd hot loop churns
// millions of tiny structs per run, and routing them through system malloc
// dominates non-JIT runtime time.
//
// Drop-in under the existing GC: the collector is RC + a NON-MOVING
// mark-sweep backstop, with metadata kept out-of-line in an address->header
// registry. Slab blocks never move, so object addresses stay stable and the
// registry is unaffected — only where the bytes come from changes. One
// instance per Runtime (Runtime is thread_local), so the allocator needs no
// locking, matching the per-Runtime GC heap.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#ifndef NDEBUG
#include <cassert>
#endif

namespace culebra {

class SlabAllocator {
 public:
  SlabAllocator() = default;
  SlabAllocator(const SlabAllocator&) = delete;
  SlabAllocator& operator=(const SlabAllocator&) = delete;

  ~SlabAllocator() {
    // High-water retention is the intended win: chunks return to the OS only
    // here, at Runtime teardown — never per object. The intrusive free-lists
    // live inside these chunks, so the chunks are the only owned allocations.
    for (void* c : chunks_) std::free(c);
  }

  // Hand out at least `n` bytes, 16-aligned. Sizes above the largest class
  // pass straight through to malloc (rare, amortized — e.g. big arrays).
  void* alloc(size_t n) {
    int idx = class_index(n);
    if (idx < 0) return std::malloc(n);
    if (void* p = free_lists_[idx]) {
      free_lists_[idx] = *reinterpret_cast<void**>(p);
      return p;
    }
    return carve(kClasses[idx]);
  }

  // Return a block obtained from this allocator. `n` MUST be the same byte
  // count passed to the matching `alloc` (sizeof(T) for a struct, capacity *
  // element size for a buffer), so it maps to the same size class.
  void free(void* p, size_t n) {
    if (!p) return;
    int idx = class_index(n);
    if (idx < 0) {
      std::free(p);
      return;
    }
#ifndef NDEBUG
    // The one way this design breaks is a block from another Runtime's slab
    // freed here (cross-allocator free). The per-thread heap + serialization
    // boundary make it impossible; assert it under the test suite anyway so a
    // future regression aborts immediately instead of corrupting a free-list.
    assert(owns(p) && "slab free of a pointer this allocator never handed out");
#endif
    *reinterpret_cast<void**>(p) = free_lists_[idx];
    free_lists_[idx] = p;
  }

 private:
  // All multiples of alignof(std::max_align_t) (16), so every carved block is
  // 16-aligned (a safe superset of what malloc/new guaranteed). JitObject
  // (~112B) -> 128, JitClosure/JitArray/JitSet -> 48, JitCell/JitTensor -> 32,
  // and the small JitValue[] / captures buffers spread across the lower
  // classes.
  static constexpr size_t kClasses[] = {16, 32, 48, 64, 128, 256, 512, 1024};
  static constexpr int kNumClasses = static_cast<int>(std::size(kClasses));
  static constexpr size_t kChunkBytes = 64 * 1024;

  // Smallest class index whose block fits `n`, or -1 if `n` exceeds the
  // largest class (caller routes those to malloc/free). A short branch ladder
  // over a fixed table — resolved at compile time for the constant struct
  // sizes, no loop on the hot path.
  static constexpr int class_index(size_t n) {
    for (int i = 0; i < kNumClasses; i++)
      if (n <= kClasses[i]) return i;
    return -1;
  }

  // Bump-carve one `block_size` block from the current chunk, refilling with a
  // fresh 64KB chunk when exhausted. A single cursor serves every class: each
  // class size is a multiple of 16 and chunks start 16-aligned, so successive
  // bumps stay aligned. The few bytes lost at a chunk's tail (< one block) are
  // negligible against 64KB.
  void* carve(size_t block_size) {
    // `!cursor_` short-circuits the first carve (cursor_/end_ start null);
    // arithmetic on a null pointer is UB even without a dereference. Once a
    // chunk is live, end_ - cursor_ is the bytes left in it.
    if (!cursor_ || static_cast<size_t>(end_ - cursor_) < block_size) {
      char* chunk = static_cast<char*>(std::malloc(kChunkBytes));
      chunks_.push_back(chunk);
      cursor_ = chunk;
      end_ = chunk + kChunkBytes;
    }
    void* p = cursor_;
    cursor_ += block_size;
    return p;
  }

#ifndef NDEBUG
  bool owns(void* p) const {
    auto* c = static_cast<char*>(p);
    for (void* base : chunks_) {
      auto* b = static_cast<char*>(base);
      if (c >= b && c < b + kChunkBytes) return true;
    }
    return false;
  }
#endif

  std::array<void*, kNumClasses> free_lists_{};  // intrusive free-list heads
  std::vector<void*> chunks_;                    // owned 64KB chunks
  char* cursor_ = nullptr;                        // bump pointer into back chunk
  char* end_ = nullptr;                           // end of the back chunk
};

}  // namespace culebra
