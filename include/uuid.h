#pragma once

// UUID generation for the `UUID` stdlib namespace.
//
// Value-neutral core (no culebra types), shared by the interp
// and the JIT/AOT slow-path adapters (stdlib_jit.h). Two
// variants, both returned in the canonical lowercase 8-4-4-4-12 hyphenated
// form: v4 (random) and v7 (Unix-millisecond timestamp prefix + random, so
// values sort by creation time — handy as database keys). Entropy comes from
// the process's shared Mersenne-Twister (culebra::random_engine(), the same
// engine `Random.*` uses and `Random.seed` reseeds) — convenient and
// reproducible under a seed, NOT cryptographically secure.

#include <chrono>
#include <cstdint>
#include <string>

#include "shared.h"  // culebra::random_engine()

namespace culebra::uuid {

// Render 16 bytes as canonical lowercase 8-4-4-4-12 hex with hyphens.
inline std::string format(const uint8_t b[16]) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
    out += hex[b[i] >> 4];
    out += hex[b[i] & 0xF];
  }
  return out;
}

// Fill `b` with 16 random bytes from the shared engine (two 64-bit draws).
inline void fill_random(uint8_t b[16]) {
  auto& eng = culebra::random_engine();
  uint64_t lo = eng(), hi = eng();
  for (int i = 0; i < 8; i++) {
    b[i] = static_cast<uint8_t>(lo >> (i * 8));
    b[i + 8] = static_cast<uint8_t>(hi >> (i * 8));
  }
}

// Version 4 (RFC 4122): 122 random bits, version nibble 4, variant bits 10.
inline std::string v4() {
  uint8_t b[16];
  fill_random(b);
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);  // variant 10xx
  return format(b);
}

// Version 7 (RFC 9562): 48-bit big-endian Unix-millisecond timestamp, then
// version 7 and 74 random bits — monotonic-ish, sorts by creation time.
inline std::string v7() {
  uint8_t b[16];
  fill_random(b);
  uint64_t ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  for (int i = 0; i < 6; i++)
    b[i] = static_cast<uint8_t>(ms >> ((5 - i) * 8));  // big-endian 48-bit
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x70);   // version 7
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);   // variant 10xx
  return format(b);
}

}  // namespace culebra::uuid
