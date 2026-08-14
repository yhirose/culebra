#pragma once

// A slot+generation id registry over borrowed/owned `T*` handles: a captured or
// escaped id (used after the handle is invalidated, or after a slot is reused)
// resolves stale and fails safely with no ABA. The id encodes (gen<<32 | slot)
// in unsigned space (a signed shift into the sign bit would be UB). Http's
// client/server/streaming sink/WebSocket connection tables, net.h's Sock
// table, sqlite.h's db/stmt tables and canvas.h's Sprite table are all one of
// these, so a forged or stale id behaves the same way everywhere: bounds- and
// generation-checked, never a dereference of arbitrary memory (same posture as
// the wrap.h foreign table and File's fd table). Header-only and value-neutral
// (no dependency on culebra Value / JitValue / GC) so it can sit under any of
// those files without pulling them into each other.

#include <cstdint>
#include <vector>

template <class T>
struct IdRegistry {
  std::vector<T*> slots;
  std::vector<uint32_t> gen;
  std::vector<uint32_t> free;
  // The one place the (gen, slot) packing lives, so a caller walking `slots`
  // to retire what it finds there names an id the same way add() minted it.
  int64_t id_at(uint32_t s) const {
    return static_cast<int64_t>((static_cast<uint64_t>(gen[s]) << 32) |
                                static_cast<uint64_t>(s));
  }
  int64_t add(T* p) {
    uint32_t s;
    if (!free.empty()) {
      s = free.back();
      free.pop_back();
      slots[s] = p;
    } else {
      s = static_cast<uint32_t>(slots.size());
      slots.push_back(p);
      gen.push_back(0);
    }
    return id_at(s);
  }
  T* get(int64_t id) {
    uint32_t s = static_cast<uint32_t>(id & 0xFFFFFFFF);
    uint32_t g = static_cast<uint32_t>(static_cast<uint64_t>(id) >> 32);
    if (s >= slots.size() || gen[s] != g) return nullptr;
    return slots[s];
  }
  // Generation-checked like get(), so retiring a stale id is a no-op rather
  // than retiring whoever holds the slot now (and free-listing it twice).
  void invalidate(int64_t id) {
    uint32_t s = static_cast<uint32_t>(id & 0xFFFFFFFF);
    uint32_t g = static_cast<uint32_t>(static_cast<uint64_t>(id) >> 32);
    if (s >= slots.size() || gen[s] != g) return;
    slots[s] = nullptr;
    gen[s]++;  // bump so a stale id (old generation) now fails
    free.push_back(s);
  }
};
