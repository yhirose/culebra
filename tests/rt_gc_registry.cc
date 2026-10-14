// The live-object registry (include/rt/gc.h's GcRegistry) on its own: how
// full it lets itself get, that births and deaths at a steady live count do
// not grow it, and that neither loses an entry.
//
//   clang++ -std=c++20 -O2 -Iinclude tests/rt_gc_registry.cc -o /tmp/gcr && /tmp/gcr

#include "rt/gc.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using culebra::gc::GcHeader;
using culebra::gc::GcRegistry;

// A Release build compiles assert out, so the checks are spelled out.
static void check(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "rt_gc_registry: FAILED: %s\n", what);
  std::exit(1);
}

// The registry never reads through a key: any 16-aligned address will do.
static void* key(size_t i) {
  return reinterpret_cast<void*>(uintptr_t(0x100000000) + i * 16);
}
static GcHeader header(size_t i) {
  return GcHeader{0, 0, 0, 0, static_cast<uint32_t>(i)};
}

int main() {
  // --- never more than 5/8 full, at any size on the way up ---
  GcRegistry grown;
  for (size_t i = 0; i < 100000; i++) {
    grown.insert(key(i), header(i));
    check(grown.size() * 8 <= grown.capacity() * 5, "over 5/8 full");
  }
  check(grown.size() == 100000, "an insert was lost");

  // --- a steady live count: the dead's slots are taken back, not added to ---
  constexpr size_t kLive = 1000, kChurn = 200000;
  GcRegistry steady;
  for (size_t i = 0; i < kLive; i++) steady.insert(key(i), header(i));
  for (size_t i = kLive; i < kLive + kChurn; i++) {
    steady.insert(key(i), header(i));
    size_t slot = steady.find_slot(key(i - kLive));  // the oldest dies
    check(slot != GcRegistry::kNpos, "a live entry was not found");
    check(steady.header_at(slot).size == i - kLive, "an entry has another's header");
    steady.erase_slot(slot);
  }
  check(steady.size() == kLive, "the live count drifted");
  check(steady.capacity() <= 4096, "churn at a steady live count grew the table");

  // --- what is left is exactly the last kLive, each with its own header ---
  for (size_t i = 0; i < kChurn; i++)
    check(steady.find(key(i)) == nullptr, "a dead entry is still found");
  for (size_t i = kChurn; i < kChurn + kLive; i++) {
    GcHeader* h = steady.find(key(i));
    check(h && h->size == i, "a survivor was lost");
  }
  size_t walked = 0;
  steady.for_each([&](void*, GcHeader&) { walked++; });
  check(walked == kLive, "the walk and the count disagree");

  std::printf("rt_gc_registry: OK  (grown cap=%zu steady cap=%zu)\n",
              grown.capacity(), steady.capacity());
  return 0;
}
