#pragma once

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace culebra {

// --- Structured runtime error ---

// Both backends throw this; try/catch machinery translates it into a
// culebra Object with `kind`/`message`/`line`/`col` fields. Inherits
// from std::runtime_error so unconverted call sites still work via
// the standard exception interface.
class CulebraError : public std::runtime_error {
 public:
  std::string kind;
  long line = 0;
  long col = 0;

  CulebraError(std::string k, std::string msg, long l = 0, long c = 0)
      : std::runtime_error(std::move(msg)),
        kind(std::move(k)),
        line(l),
        col(c) {}
};

// Throw TypeError "takes N positional argument(s) but M given" when M
// exceeds the cap. `cap < 0` means no `*` separator (no cap); `cap ==
// 0` means a leading `*` (variadic via __ARGS__, no overflow error).
// Shared by interp's bind_call_args, the JIT static kwargs resolver,
// and the JIT dynamic-callee runtime guard — all three throw the same
// shape.
inline void throw_if_too_many_positionals(long cap, long n_pos,
                                           long line, long col) {
  if (cap <= 0 || n_pos <= cap) return;
  throw CulebraError("TypeError", std::format(
      "takes {} positional argument{} but {} given",
      cap, cap == 1 ? "" : "s", n_pos), line, col);
}

// --- Numeric formatting / parsing ---

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

// JSON.stringify quote escaping. Shared by both backends' stringify
// implementations. Control bytes (<0x20) emit as `\u00xx`.
inline std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}

[[noreturn]] inline void throw_type_error_at(long line, long col) {
  throw CulebraError("TypeError",
                     std::format("type error at {}:{}.", line, col), line,
                     col);
}

// Integer power by squaring. `exp` must be non-negative; result wraps
// on overflow (matches the rest of Long arithmetic — no bignum).
inline long ipow_nonneg(long base, long exp) {
  long r = 1;
  while (exp > 0) {
    if (exp & 1) r *= base;
    exp >>= 1;
    if (exp > 0) base *= base;
  }
  return r;
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

// --- Runtime context ---

// Owns all per-instance VM state. Multiple Runtimes can coexist on the
// same thread; RuntimeScope swaps the active one. Single-Runtime
// embedders never construct one — a lazy thread-local default fills in.
//
// Heavy types (InterpGC, ShapeRegistry, ...) live in type-erased slots
// because they're defined in headers that include shared.h, so they
// can't be direct members.
enum RuntimeSlot : size_t {
  kSlotInterpGc = 0,
  kSlotJitGc,
  kSlotShapeRegistry,
  kSlotDeferStack,
  kSlotJitHooks,
  kRuntimeSlotCount
};

struct Runtime {
  std::mt19937_64 random_engine{std::random_device{}()};
  std::vector<std::string> sys_argv;

  // JIT exception carriers. Set by `culebra_runtime_throw`, read by
  // the try/catch landing pad. See jit.h for the protocol.
  int8_t thrown_tag = 0;
  int64_t thrown_data = 0;
  int8_t is_throw = 0;

  std::array<void*, kRuntimeSlotCount> substate{};
  std::array<void (*)(void*), kRuntimeSlotCount> substate_deleter{};

  Runtime() = default;
  ~Runtime() {
    for (size_t i = 0; i < substate.size(); ++i) {
      if (substate[i] && substate_deleter[i]) substate_deleter[i](substate[i]);
    }
  }
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
};

inline thread_local Runtime* _culebra_current_runtime = nullptr;

inline Runtime& default_runtime() {
  static thread_local Runtime rt;
  return rt;
}

inline Runtime& current_runtime() {
  return _culebra_current_runtime ? *_culebra_current_runtime
                                  : default_runtime();
}

// RAII to switch the active Runtime for the current thread.
struct RuntimeScope {
  Runtime* prev_;
  explicit RuntimeScope(Runtime& rt) : prev_(_culebra_current_runtime) {
    _culebra_current_runtime = &rt;
  }
  ~RuntimeScope() { _culebra_current_runtime = prev_; }
  RuntimeScope(const RuntimeScope&) = delete;
  RuntimeScope& operator=(const RuntimeScope&) = delete;
};

// Lazy-init a default-constructible T into a Runtime slot.
template <class T>
inline T& runtime_substate(RuntimeSlot slot) {
  auto& rt = current_runtime();
  if (!rt.substate[slot]) {
    rt.substate[slot] = new T();
    rt.substate_deleter[slot] = [](void* p) { delete static_cast<T*>(p); };
  }
  return *static_cast<T*>(rt.substate[slot]);
}

// --- Shared PRNG (interpreter and JIT) ---

inline std::mt19937_64& random_engine() {
  return current_runtime().random_engine;
}

// --- Well-known property contract ---

// Property names the runtime invokes behind the scenes:
//   drop  - RAII destructor (called on last release / cycle break)
//   iter  - iterator constructor (returns an object with `next`)
//   next  - iterator advance (returns `{done, value}`)
// Each must be a 0-arg Function. The name set and error wording live
// here so the two backends can't drift; per-backend type checks stay
// in each backend (Value vs JitClosure aren't interchangeable).
inline bool is_well_known_prop(std::string_view name) {
  return name == "drop" || name == "iter" || name == "next";
}

[[noreturn]] inline void throw_well_known_prop_contract_error(
    std::string_view name) {
  throw CulebraError(
      "DropContractError",
      std::format("type error: '{}' must be a Function taking no arguments.",
                  name));
}

}  // namespace culebra
