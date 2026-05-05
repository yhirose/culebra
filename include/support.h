#pragma once

#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace culebra {

// ---------------------------------------------------------------------------
// Numeric formatting / parsing
// ---------------------------------------------------------------------------

// Shortest round-trip decimal for a double, with a forced decimal point
// or exponent so Float display is visually distinguishable from Long
// (`1.0`, `0.5`, `-2.5`, `1e-05`, `nan`, `inf`, `-inf`). Shared by the
// interpreter's `Value::str_float` and the JIT's `_culebra_value_to_str_impl`.
inline std::string format_float_shortest(double d) {
  if (std::isnan(d)) return "nan";
  if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
  char buf[32];
  auto* end =
      std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::general).ptr;
  std::string s(buf, end);
  if (s.find('.') == std::string::npos &&
      s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos) {
    s += ".0";
  }
  return s;
}

[[noreturn]] inline void throw_type_error_at(long line, long col) {
  throw std::runtime_error(std::format("type error at {}:{}.", line, col));
}

// Trim ASCII whitespace from both ends of a string view, returning the
// substring. Shared by the numeric string parsers below.
inline std::string_view trim_ascii(std::string_view s) {
  size_t i = 0, j = s.size();
  while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) i++;
  while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) j--;
  return s.substr(i, j - i);
}

// Parse a full string as a base-10 signed long; whitespace-trim allowed,
// any other trailing content or invalid form throws `type error at L:C`.
inline long parse_long_strict(std::string_view s, long line, long col) {
  auto t = trim_ascii(s);
  if (t.empty()) throw_type_error_at(line, col);
  try {
    size_t used = 0;
    std::string owned(t);
    long v = std::stol(owned, &used, 10);
    if (used != owned.size()) throw std::invalid_argument("");
    return v;
  } catch (const std::runtime_error&) {
    throw;
  } catch (...) {
    throw_type_error_at(line, col);
  }
  return 0;  // unreachable
}

// Parse a full string as a double; same trim / full-consumption rules.
inline double parse_double_strict(std::string_view s, long line, long col) {
  auto t = trim_ascii(s);
  if (t.empty()) throw_type_error_at(line, col);
  try {
    size_t used = 0;
    std::string owned(t);
    double v = std::stod(owned, &used);
    if (used != owned.size()) throw std::invalid_argument("");
    return v;
  } catch (const std::runtime_error&) {
    throw;
  } catch (...) {
    throw_type_error_at(line, col);
  }
  return 0.0;  // unreachable
}

// ---------------------------------------------------------------------------
// Shared PRNG (interpreter and JIT)
// ---------------------------------------------------------------------------

// Default seed state for the shared PRNG. Produced once at program
// startup via a seed-sequence function call so that multiple engines
// (if any) are uncorrelated. Not thread-safe — Culebra's execution
// model is single-threaded.
inline std::mt19937_64 _culebra_random_engine{std::random_device{}()};

// Single process-wide PRNG, shared between the interpreter and the
// JIT so `Random.seed(n)` has the same effect regardless of backend.
inline std::mt19937_64& random_engine() { return _culebra_random_engine; }

// ---------------------------------------------------------------------------
// Well-known property contract
// ---------------------------------------------------------------------------

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
