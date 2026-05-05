#pragma once

#include <format>
#include <stdexcept>
#include <string_view>

namespace culebra {

// Property names the runtime invokes behind the scenes:
//   drop  - RAII destructor (called on last release / cycle break)
//   iter  - iterator constructor (returns an object with `next`)
//   next  - iterator advance (returns `{done, value}`)
// Each must be a 0-arg Function. Both backends enforce the shape at
// assignment time so a violation surfaces near its source. Type-shape
// checking stays in each backend (interpreter Value vs JitClosure are
// not interchangeable); only the name set and error wording are shared
// here so the two backends can't drift.
inline bool is_well_known_prop(std::string_view name) {
  return name == "drop" || name == "iter" || name == "next";
}

[[noreturn]] inline void throw_well_known_prop_contract_error(
    std::string_view name) {
  throw std::runtime_error(std::format(
      "type error: '{}' must be a Function taking no arguments.", name));
}

}  // namespace culebra
