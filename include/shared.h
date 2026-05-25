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

// Shared by interp's `eval_destructure_assign` and JIT's
// `compile_destructure_assign` so both backends report the same
// structured error (`ValueError` + descriptive message + location)
// when an Object / Array / Tuple pattern fails to match its rval.
[[noreturn]] inline void throw_destructure_mismatch_at(long line, long col) {
  throw CulebraError("ValueError",
                     std::format(
                         "destructure pattern did not match value at {}:{}.",
                         line, col),
                     line, col);
}

// `o.x += rhs` against a missing property `x`. Both backends used to
// diverge here — interp checked existence and threw AttributeError,
// JIT read the missing slot as nil and then threw TypeError on
// `nil + rhs`. This helper unifies the kind + message + location.
[[noreturn]] inline void throw_compound_missing_property_at(
    long line, long col) {
  throw CulebraError("AttributeError",
                     "compound assignment on missing property.",
                     line, col);
}

// Reassigning a `let` (non-mut) binding. Interp tracks per-binding
// mut via the `Symbol`'s `mut` flag; the JIT carries `mut` on
// `VarSlot` and routes here when a write hits a non-mut slot.
[[noreturn]] inline void throw_immutable_assign_at(
    const std::string& name, long line, long col) {
  throw CulebraError("ImmutableError",
                     std::format("cannot reassign '{}' (declared without 'mut')",
                                 name),
                     line, col);
}

// Call site passed a keyword the callee doesn't accept. Both backends
// used to diverge: interp threw at runtime (catchable by try/catch),
// JIT raised at IR-emit time (uncatchable). This helper unifies them
// to runtime throws.
[[noreturn]] inline void throw_unknown_kwarg_at(
    const std::string& name, long line, long col) {
  throw CulebraError("TypeError",
                     std::format("unknown keyword argument '{}'", name),
                     line, col);
}

// Call site failed to bind a required parameter (no positional, no
// kwarg, no default). Same backend-asymmetry rationale as the unknown-
// kwarg helper above.
[[noreturn]] inline void throw_missing_required_arg_at(
    const std::string& name, long line, long col) {
  throw CulebraError("ArityError",
                     std::format("missing required argument '{}'", name),
                     line, col);
}

// Generic runtime throw for cases where the JIT used to detect the
// error at IR-emit time (uncatchable) while the interp threw at
// eval time (catchable). Both backends now route through this helper
// to keep `try { ... } catch e { e.kind }` semantics symmetric.
[[noreturn]] inline void throw_runtime_error_at(
    const std::string& kind, const std::string& msg,
    long line, long col) {
  throw CulebraError(kind, msg, line, col);
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

// Split a pipe-separated Union type annotation into its component
// type names. `Long | Float` → ["Long", "Float"]. Whitespace around
// each candidate is trimmed. A single type name (no pipe) returns
// the input unchanged as the only element. Empty input yields an
// empty vector. Shared by both backends' type checks.
//
// DEFENSIVE: empty alternatives (e.g. `Long ||  | Float`, leading
// `|`, trailing `|`) are silently skipped. Grammar prevents these
// from reaching here in normal source, but the helper is also
// called on canonicalized strings during multifn dedup so we don't
// rely on grammar alone.
//
// LIFETIME: each returned string_view aliases bytes inside `name`;
// the caller must keep `name`'s underlying storage alive while
// using the returned views. Do not pass a temporary.
inline std::vector<std::string_view> split_union_types(
    std::string_view name) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (start <= name.size()) {
    size_t end = name.find('|', start);
    if (end == std::string_view::npos) end = name.size();
    auto candidate = trim_ascii(name.substr(start, end - start));
    if (!candidate.empty()) out.push_back(candidate);
    if (end == name.size()) break;
    start = end + 1;
  }
  return out;
}

// Canonical form for a (possibly Union) type annotation, used in
// fn.params introspection and multifn redeclaration matching so
// whitespace variants of the same Union compare equal.
// `Long|Float` and `Long  |  Float` both → `Long | Float`.
// Empty input returns "". Non-Union names are returned unchanged.
// Owned std::string so storage outlives `name`.
inline std::string canonicalize_type_annotation(std::string_view name) {
  if (name.find('|') == std::string_view::npos) {
    return std::string(trim_ascii(name));
  }
  std::string out;
  // Drop duplicate alternatives keeping first-occurrence order, so
  // `Long | Long` collapses to `Long` and `Float | Long | Float` to
  // `Float | Long`. A handful of alts is normal — linear scan is fine.
  std::vector<std::string_view> seen;
  for (auto cand : split_union_types(name)) {
    bool dup = false;
    for (auto s : seen) if (s == cand) { dup = true; break; }
    if (dup) continue;
    seen.push_back(cand);
    if (!out.empty()) out += " | ";
    out.append(cand.data(), cand.size());
  }
  return out;
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
  kSlotJitModuleTable,
  kSlotJitNamespaceTable,
  kSlotTestRegistry,
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
