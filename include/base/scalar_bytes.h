#pragma once

// Fixed-width scalars <-> bytes in an explicit byte order: the kernel under
// `String.pack` / `String.unpack`. Engine-neutral, and silent about errors:
// it answers what fits and what the bytes are, and each caller words the
// failure for what it was handed (an Array element, a record field).

#include <bit>
#include <cmath>
#include <cstdint>
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
inline const Type* find_type(std::string_view name) {
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

// The Long range an integer kind holds (i64: all of it).
inline std::pair<int64_t, int64_t> int_range(const Type& t) {
  if (t.width == 8) return {std::numeric_limits<int64_t>::min(),
                            std::numeric_limits<int64_t>::max()};
  const int bits = t.width * 8;
  if (t.kind == Kind::Signed)
    return {-(int64_t{1} << (bits - 1)), (int64_t{1} << (bits - 1)) - 1};
  return {0, (int64_t{1} << bits) - 1};
}

// The low `width` bytes of `bits`, least significant first unless `big`.
inline void store_bits(uint8_t* p, uint64_t bits, int width, bool big) {
  for (int k = 0; k < width; k++) {
    uint8_t b = static_cast<uint8_t>(bits >> (8 * k));
    p[big ? width - 1 - k : k] = b;
  }
}
inline uint64_t load_bits(const uint8_t* p, int width, bool big) {
  uint64_t bits = 0;
  for (int k = 0; k < width; k++)
    bits |= static_cast<uint64_t>(p[big ? width - 1 - k : k]) << (8 * k);
  return bits;
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

inline void store_float(uint8_t* p, const Type& t, double d, bool big) {
  if (t.width == 8) {
    store_bits(p, std::bit_cast<uint64_t>(d), 8, big);
    return;
  }
  store_bits(p, std::bit_cast<uint32_t>(static_cast<float>(round_f32(d))), 4,
             big);
}

inline int64_t load_int(const uint8_t* p, const Type& t, bool big) {
  const int shift = t.kind == Kind::Signed ? 64 - t.width * 8 : 0;
  return static_cast<int64_t>(load_bits(p, t.width, big) << shift) >> shift;
}
inline double load_float(const uint8_t* p, const Type& t, bool big) {
  uint64_t bits = load_bits(p, t.width, big);
  if (t.width == 8) return std::bit_cast<double>(bits);
  return static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(bits)));
}

}  // namespace culebra::scalar_bytes
