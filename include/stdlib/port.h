#pragma once

// A TCP/UDP port number the socket layer can take. Net and Http hand ports
// to getaddrinfo / httplib as an `int`, where 70000 would wrap to 4464 and
// 1 << 32 to 0 (an ephemeral bind) — each a silent success. The constructor
// is private, so a number the program supplied reaches that layer only
// through `checked`, and every entry that forgets to check fails to compile.

#include <cstdint>
#include <string_view>

#include <base/shared.h>  // CulebraError / culebra::format

namespace culebra {

class Port {
 public:
  // `ctx` names the entry (`Net.listen`), as its other errors do.
  static Port checked(int64_t v, std::string_view ctx, int64_t line,
                      int64_t col) {
    if (v < 0 || v > 65535)
      throw CulebraError(
          "ValueError",
          culebra::format("{}: port must be between 0 and 65535, got {}", ctx,
                          v),
          line, col);
    return Port(static_cast<uint16_t>(v));
  }
  // 0: bind to a port the OS picks.
  static constexpr Port any() { return Port(0); }

  uint16_t value() const { return v_; }

 private:
  constexpr explicit Port(uint16_t v) : v_(v) {}
  uint16_t v_;
};

}  // namespace culebra
