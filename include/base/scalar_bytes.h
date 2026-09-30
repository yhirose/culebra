#pragma once

// Fixed-width scalars <-> bytes in an explicit byte order: the kernel under
// `String.pack` / `String.unpack`. Engine-neutral, and silent about errors:
// it answers what fits and what the bytes are, and each caller words the
// failure for what it was handed (an Array element, a record field).

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace culebra::scalar_bytes {

enum class Kind : uint8_t { Unsigned, Signed, Float, Bool };

struct Type {
  std::string_view name;
  uint8_t width;
  Kind kind;
};

// No u64: its upper half is not a Long. `i64` carries the same bits.
inline constexpr Type kTypes[] = {
    {"u8", 1, Kind::Unsigned},  {"i8", 1, Kind::Signed},
    {"u16", 2, Kind::Unsigned}, {"i16", 2, Kind::Signed},
    {"u32", 4, Kind::Unsigned}, {"i32", 4, Kind::Signed},
    {"i64", 8, Kind::Signed},   {"f32", 4, Kind::Float},
    {"f64", 8, Kind::Float},    {"bool", 1, Kind::Bool},
};

// Null when `name` is none of kTypes.
constexpr const Type* find_type(std::string_view name) {
  for (const Type& t : kTypes)
    if (t.name == name) return &t;
  return nullptr;
}
// "u8, i8, ..." for a message naming the choices.
inline std::string type_names() {
  std::string s;
  for (const Type& t : kTypes) {
    if (!s.empty()) s += ", ";
    s += t.name;
  }
  return s;
}

// What a value of this kind is, for a message that one was not.
constexpr std::string_view value_kind_name(Kind k) {
  return k == Kind::Bool ? "Bool" : k == Kind::Float ? "Long or Float" : "Long";
}

// The Long range a type's integer holds (i64: all of it). Asked of a float
// or bool type it answers something, which nothing reads.
inline std::pair<int64_t, int64_t> int_range(const Type& t) {
  if (t.width == 8) return {std::numeric_limits<int64_t>::min(),
                            std::numeric_limits<int64_t>::max()};
  const int bits = t.width * 8;
  if (t.kind == Kind::Signed)
    return {-(int64_t{1} << (bits - 1)), (int64_t{1} << (bits - 1)) - 1};
  return {0, (int64_t{1} << bits) - 1};
}

// One unsigned word of type U at `p` in the stated byte order: a plain
// (unaligned) copy, byte-swapped only when that order is not the host's.
template <class U>
inline U load_as(const uint8_t* p, bool big) {
  U v;
  std::memcpy(&v, p, sizeof v);
  return big == (std::endian::native == std::endian::big) ? v : std::byteswap(v);
}
template <class U>
inline void store_as(uint8_t* p, U v, bool big) {
  if (big != (std::endian::native == std::endian::big)) v = std::byteswap(v);
  std::memcpy(p, &v, sizeof v);
}
// Calls f with a value of the unsigned word type `width` bytes wide, so a
// loop over many scalars picks its width once, outside the loop.
template <class F>
inline decltype(auto) with_word(int width, F&& f) {
  switch (width) {
    case 1: return f(uint8_t{});
    case 2: return f(uint16_t{});
    case 4: return f(uint32_t{});
    default: return f(uint64_t{});
  }
}

// Round to the nearest binary32 (`Math.f32`, and the Core IR's tofloat32 —
// cpp-vmlib's Op::ToFloat32): up to the rounding midpoint past float's max
// it lands on the max, beyond it on infinity; NaN stays. Spelled out because
// converting a double outside float's range is undefined.
inline double round_f32(double d) {
  constexpr double kFloatMax =
      static_cast<double>(std::numeric_limits<float>::max());
  constexpr double kFloatOverflow = 0x1.ffffffp127;  // (2-2^-24)*2^127
  if (std::isnan(d)) return d;
  if (d >= kFloatOverflow) return std::numeric_limits<double>::infinity();
  if (d > kFloatMax) return kFloatMax;
  if (d <= -kFloatOverflow) return -std::numeric_limits<double>::infinity();
  if (d < -kFloatMax) return -kFloatMax;
  return static_cast<double>(static_cast<float>(d));
}

}  // namespace culebra::scalar_bytes
