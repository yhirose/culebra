#pragma once

// What Shared.new makes of a value: five flat arrays, read by any number of
// isolates and written by none once the freeze is over. A value is a cell laid
// out as the runtime's own (tag, data): a scalar's cell is the value itself, a
// String's says where its bytes are, a container's which node it is. Nothing
// in here knows what a tag means; the runtime that made the tree does
// (conc/sendable.h). Positions are indexes, never pointers, so the arrays may
// grow while the tree is built and are let go of in one piece.

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace culebra {

struct FrozenCell {
  int64_t tag;
  int64_t data;  // a String: where in `bytes`; a container: which node
};

enum class FrozenKind : uint8_t { Leaf, Array, Tuple, Set, Object };

inline constexpr uint32_t kFrozenNone = UINT32_MAX;

// One container. The root is a node too when it is none (a Leaf: the one cell
// it has is the value, and it yields nothing).
struct FrozenNode {
  FrozenKind kind;
  uint32_t count;                // the values it yields
  uint32_t cells;                // the first of them
  uint32_t shape = kFrozenNone;  // an Object's, a Set's
};

// What the Objects with one sequence of keys share, and what a Set's members
// are found through (a Set's keys are its own cells).
struct FrozenShape {
  int64_t name = -1;            // a class meta: its names, in `bytes`
  uint32_t keys;                // the first key cell
  uint32_t count;
  uint32_t slots;               // in `index`: mask + 1 hash slots, a position + 1 each
  uint32_t mask;
  uint32_t others;              // in `index`: the positions of the keys not hashed
  uint32_t n_others;
  uint32_t mut = kFrozenNone;   // in `index`: a bit a key (an Object's)
  uint32_t meta = kFrozenNone;  // a class instance: its class's node
  int8_t dir_kind = 0;          // a class meta's (JitSpecialTable::dir_kind)
  bool is_error = false;
  bool has_drop = false;        // a class instance's drop gate
};

struct FrozenTree {
  std::vector<FrozenCell> cells;
  std::vector<FrozenNode> nodes;  // 0 is the root
  std::vector<FrozenShape> shapes;
  std::vector<uint32_t> index;
  std::vector<char> bytes;  // a String: its length, its bytes, a NUL

  std::string_view str(int64_t at) const {
    uint64_t size;
    std::memcpy(&size, bytes.data() + at, sizeof size);
    return {bytes.data() + at + sizeof size, static_cast<size_t>(size)};
  }
  const char* c_str(int64_t at) const {
    return bytes.data() + at + sizeof(uint64_t);
  }
  int64_t add_str(std::string_view s) {
    auto at = static_cast<int64_t>(bytes.size());
    uint64_t size = s.size();
    bytes.resize(bytes.size() + sizeof size + s.size() + 1);
    std::memcpy(bytes.data() + at, &size, sizeof size);
    std::memcpy(bytes.data() + at + sizeof size, s.data(), s.size());
    return at;  // the resize zeroed the NUL
  }

  const FrozenCell* cells_of(const FrozenNode& n) const {
    return cells.data() + n.cells;
  }
  const FrozenCell* keys_of(const FrozenShape& s) const {
    return cells.data() + s.keys;
  }
  // Whether an Object's key at `at` takes a write.
  bool mut_at(const FrozenShape& s, size_t at) const {
    return ((index[s.mut + at / 32] >> (at % 32)) & 1) != 0;
  }
};

}  // namespace culebra
