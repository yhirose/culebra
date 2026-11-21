#pragma once

// A slot+generation id registry over borrowed/owned `T*` handles: a captured or
// escaped id (used after the handle is invalidated, or after a slot is reused)
// resolves stale and fails safely with no ABA. The id encodes (gen<<32 | slot)
// in unsigned space (a signed shift into the sign bit would be UB). Http's
// client/server/streaming sink tables, ws_link.h's connections, net.h's Sock
// table, sqlite.h's db/stmt tables and canvas.h's Sprite table are all one of
// these, so a forged or stale id behaves the same way everywhere: bounds- and
// generation-checked, never a dereference of arbitrary memory (same posture as
// the wrap.h foreign table and File's fd table). Header-only and value-neutral
// (no dependency on culebra Value / JitValue / GC) so it can sit under any of
// those files without pulling them into each other.

#include <cassert>
#include <cstdint>
#include <deque>
#include <vector>

#include <base/held.h>

template <class T>
struct IdRegistry {
  std::vector<T*> slots;
  std::vector<uint32_t> gen;
  std::vector<uint32_t> free;
  // How many native frames hold each slot's object (use below). A deque, so a
  // Hold's pointer into it survives a registration from program code meanwhile.
  std::deque<uint32_t> users;

  // The object `id` names, held for as long as the caller keeps the Held, or an
  // empty Held for a stale id. A frame that calls program code with the object
  // in hand takes it this way, so the code cannot close it under the frame.
  culebra::Held<T> use(int64_t id) {
    uint32_t s;
    if (!live(id, &s)) return {};
    return {slots[s], culebra::Hold(&users[s])};
  }
  // How many native frames hold the object (0 for a stale id).
  uint32_t holders(int64_t id) const {
    uint32_t s;
    return live(id, &s) ? users[s] : 0;
  }
  // Whether a native frame is using the object: closing it now would free it
  // under that frame, so a close checks this first.
  bool in_use(int64_t id) const { return holders(id) != 0; }
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
      users.push_back(0);
    }
    return id_at(s);
  }
  T* get(int64_t id) {
    uint32_t s;
    return live(id, &s) ? slots[s] : nullptr;
  }
  // Generation-checked like get(), so retiring a stale id is a no-op rather
  // than retiring whoever holds the slot now (and free-listing it twice).
  void invalidate(int64_t id) {
    uint32_t s;
    if (!live(id, &s)) return;
    assert(users[s] == 0 && "an object retired while a native frame uses it");
    slots[s] = nullptr;
    gen[s]++;  // bump so a stale id (old generation) now fails
    free.push_back(s);
  }

 private:
  bool live(int64_t id, uint32_t* s) const {
    *s = static_cast<uint32_t>(id & 0xFFFFFFFF);
    uint32_t g = static_cast<uint32_t>(static_cast<uint64_t>(id) >> 32);
    return *s < slots.size() && gen[*s] == g;
  }
};
