#pragma once

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <random>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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

// ASCII-only case fold. Range-checked so the result is locale-
// independent — std::toupper/tolower consult the C locale and would
// fold Latin-1 bytes too under e.g. en_US.UTF-8. Non-ASCII bytes
// (>= 0x80) pass through unchanged, matching the "ASCII uppercase /
// lowercase" contract in docs/language.md §17.1.
inline std::string ascii_upper(std::string s) {
  for (auto& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return s;
}
inline std::string ascii_lower(std::string s) {
  for (auto& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

// True iff `name` has a `|` at the outermost bracket depth (i.e. a
// top-level Union alt separator). `Array<Long | Float>` returns
// false — the `|` lives inside `<...>`. Callers gate the Union
// branch on this so a single Generic-with-inner-Union doesn't
// recurse forever via the otherwise-safe `find('|') != npos` check.
inline bool has_toplevel_pipe(std::string_view name) {
  int depth = 0;
  for (char c : name) {
    if (c == '<') depth++;
    else if (c == '>') { if (depth > 0) depth--; }
    else if (c == '|' && depth == 0) return true;
  }
  return false;
}

// Split a pipe-separated Union type annotation into its component
// type names. `Long | Float` → ["Long", "Float"]. Whitespace around
// each candidate is trimmed. A single type name (no pipe) returns
// the input unchanged as the only element. Empty input yields an
// empty vector. Shared by both backends' type checks.
//
// Generic-aware: `|` inside `<...>` does not split, so
// `Array<Long | Float>` stays one candidate. Depth is tracked across
// the whole string; unbalanced `>` are clamped to 0.
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
  int depth = 0;
  for (size_t i = 0; i < name.size(); i++) {
    char c = name[i];
    if (c == '<') depth++;
    else if (c == '>') { if (depth > 0) depth--; }
    else if (c == '|' && depth == 0) {
      auto cand = trim_ascii(name.substr(start, i - start));
      if (!cand.empty()) out.push_back(cand);
      start = i + 1;
    }
  }
  auto last = trim_ascii(name.substr(start));
  if (!last.empty()) out.push_back(last);
  return out;
}

// Split a comma-separated Generic argument list into its components,
// respecting nested `<...>`. `Long, Map<String, Long>` →
// ["Long", "Map<String, Long>"]. Empty args yield empty vector.
inline std::vector<std::string_view> split_generic_args(
    std::string_view args) {
  std::vector<std::string_view> out;
  size_t start = 0;
  int depth = 0;
  for (size_t i = 0; i < args.size(); i++) {
    char c = args[i];
    if (c == '<') depth++;
    else if (c == '>') { if (depth > 0) depth--; }
    else if (c == ',' && depth == 0) {
      auto cand = trim_ascii(args.substr(start, i - start));
      if (!cand.empty()) out.push_back(cand);
      start = i + 1;
    }
  }
  auto last = trim_ascii(args.substr(start));
  if (!last.empty()) out.push_back(last);
  return out;
}

// Split a single (non-Union) type name into outer + generic args.
// `Array<Long>` → {"Array", "Long"}; `Map<String, Long>` →
// {"Map", "String, Long"}; `Long` → {"Long", ""}.
// The args view is the raw inside-the-brackets text (callers split
// it further with `split_generic_args`).
//
// DEFENSIVE: malformed inputs (`<T>` with empty outer, `Array<Long>>`
// or `Array<<Long>` with unbalanced brackets) fall back to the whole
// trimmed string as the outer with no args, rather than yielding a
// degenerate split. Grammar normally prevents these from reaching
// here, but the helper is exported and may be called on
// programmatically-constructed type strings.
//
// LIFETIME: outer and args alias bytes inside `name`; caller must
// keep `name`'s underlying storage alive while using them. Do not
// pass a temporary std::string.
struct GenericHead {
  std::string_view outer;
  std::string_view args;  // empty if no <...>
};
inline GenericHead parse_generic_head(std::string_view name) {
  auto trimmed = trim_ascii(name);
  if (trimmed.empty()) return {trimmed, {}};
  auto pos = trimmed.find('<');
  if (pos == std::string_view::npos || trimmed.back() != '>') {
    return {trimmed, {}};
  }
  // Outer name must be non-empty.
  if (pos == 0) return {trimmed, {}};
  // Brackets must balance — reject `Array<Long>>` and `Array<<Long>`.
  int depth = 0;
  for (char c : trimmed) {
    if (c == '<') depth++;
    else if (c == '>') {
      depth--;
      if (depth < 0) return {trimmed, {}};
    }
  }
  if (depth != 0) return {trimmed, {}};
  return {
    trim_ascii(trimmed.substr(0, pos)),
    trimmed.substr(pos + 1, trimmed.size() - pos - 2),
  };
}

// Parse a type-parameter declaration like `T` or `T: Comparable` into
// its name and (possibly empty) bound. Used by class / trait / fn
// declarations to extract Generic params from CLASS_HEAD tokens.
struct TypeParam {
  std::string_view name;
  std::string_view bound;   // empty = no bound
};
inline TypeParam parse_type_param(std::string_view raw) {
  auto trimmed = trim_ascii(raw);
  auto pos = trimmed.find(':');
  if (pos == std::string_view::npos) return {trimmed, {}};
  return {trim_ascii(trimmed.substr(0, pos)),
          trim_ascii(trimmed.substr(pos + 1))};
}

// Rewrite every occurrence of a class type-param name (`T`, `K`, ...)
// inside `tn` with "Any", including nested forms like `Array<T>` and
// `T | Long`. Used by class-method neutralization so the runtime type
// check is no-op for the documented parameters. Returns the rewritten
// string in canonical form (single-space `|`, comma-space-separated
// generic args).
inline std::string rewrite_type_params_to_any(
    std::string_view tn,
    const std::vector<std::string_view>& type_params) {
  auto trimmed = trim_ascii(tn);
  if (trimmed.empty()) return "";
  // Top-level Union (depth-aware): rewrite each alt, join with " | ".
  if (has_toplevel_pipe(trimmed)) {
    std::string out;
    bool first = true;
    for (auto cand : split_union_types(trimmed)) {
      if (!first) out += " | ";
      out += rewrite_type_params_to_any(cand, type_params);
      first = false;
    }
    return out;
  }
  auto head = parse_generic_head(trimmed);
  // Outer alone matches a type-param: the whole annotation becomes Any
  // (so `T`, `T<Long>`, etc. all collapse to "Any" — the args were
  // documentation anyway). Type-params may carry a bound (`T: Foo`);
  // strip via parse_type_param so comparison sees just the name.
  for (auto tp_raw : type_params) {
    if (head.outer == parse_type_param(tp_raw).name) return "Any";
  }
  if (head.args.empty()) return std::string(head.outer);
  // Generic: keep outer, recurse into each arg.
  std::string out(head.outer);
  out += '<';
  bool first = true;
  for (auto a : split_generic_args(head.args)) {
    if (!first) out += ", ";
    out += rewrite_type_params_to_any(a, type_params);
    first = false;
  }
  out += '>';
  return out;
}

// Canonical form for a (possibly Union, possibly Generic) type
// annotation, used in fn.params introspection and multifn
// redeclaration matching so whitespace variants of the same
// type compare equal.
//
// `Long|Float`               → `Long | Float`
// `Array<Long|Float>`        → `Array<Long | Float>`
// `Array < Long , Float >`   → `Array<Long, Float>`
// `Long | Long`              → `Long`         (dedup at top level)
// Empty input                → ""
//
// Recursive: nested `<...>` are canonicalized as well. Owned
// std::string so storage outlives `name`.
inline std::string canonicalize_type_annotation(std::string_view name) {
  auto trimmed = trim_ascii(name);
  if (trimmed.empty()) return "";

  auto cands = split_union_types(trimmed);
  if (cands.size() > 1) {
    // Top-level Union — canonicalize each candidate and dedup. Linear
    // dedup against canonical forms covers `Array<Long> | Array<Long>`
    // (different whitespace inside the args still collapses).
    std::string out;
    std::vector<std::string> seen;
    for (auto cand : cands) {
      auto canon = canonicalize_type_annotation(cand);
      bool dup = false;
      for (auto& s : seen) if (s == canon) { dup = true; break; }
      if (dup) continue;
      seen.push_back(canon);
      if (!out.empty()) out += " | ";
      out += canon;
    }
    return out;
  }

  // Single type — check for Generic.
  auto head = parse_generic_head(trimmed);
  if (head.args.empty()) return std::string(head.outer);

  std::string out(head.outer);
  out += '<';
  bool first = true;
  for (auto a : split_generic_args(head.args)) {
    if (!first) out += ", ";
    out += canonicalize_type_annotation(a);
    first = false;
  }
  out += '>';
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

// --- trait / protocol registry (§15) ---
//
// A trait declares a set of required methods. A class conforms to a
// trait when it has every required method (matching arity) — Go /
// Python `__str__` style structural conformance, no `impl` block.
// Default methods (`has_default = true`) are supplied by the trait
// itself and do NOT need to be on the class.

struct TraitMethod {
  std::string name;
  size_t arity;          // declared positional arity (excluding `this`)
  bool has_default;
};

struct TraitDef {
  std::string name;
  std::vector<TraitMethod> methods;
};

// Process-wide registry. Trait declarations register here; type_matches
// / multifn_specificity consult it on each lookup. Keyed by trait name.
// Guarded by trait_mutex(): host threads can declare traits and look
// them up concurrently (mt_smoke exercises this).
inline std::shared_mutex& trait_mutex() {
  static std::shared_mutex m;
  return m;
}

inline std::unordered_map<std::string, TraitDef>& trait_registry() {
  static std::unordered_map<std::string, TraitDef> reg;
  return reg;
}

// Cache: class name → trait name → conforms? Populated lazily by
// type_matches when it encounters a value of a known class against a
// known trait. Cleared on new trait registration (later traits may flip
// earlier "no" answers for already-cached classes). Shares trait_mutex().
inline std::unordered_map<std::string,
                          std::unordered_map<std::string, bool>>&
trait_conformance_cache() {
  static std::unordered_map<std::string,
                            std::unordered_map<std::string, bool>>
      cache;
  return cache;
}

inline void register_trait(TraitDef def) {
  std::unique_lock lock(trait_mutex());
  trait_registry()[def.name] = std::move(def);
  trait_conformance_cache().clear();
}

inline const TraitDef* lookup_trait(std::string_view name) {
  std::shared_lock lock(trait_mutex());
  auto& reg = trait_registry();
  auto it = reg.find(std::string(name));
  return it == reg.end() ? nullptr : &it->second;
}

// Snapshot every registered trait name under a read lock so the caller
// can iterate the names without holding trait_mutex. Used by interp /
// JIT multifn warm-up where the inner `type_matches` call re-acquires
// the mutex for cache writes — holding it across the loop would
// deadlock. Returns an empty vector with no allocation if the registry
// is empty (= the common case for trait-free programs).
inline std::vector<std::string> snapshot_trait_names() {
  std::shared_lock lock(trait_mutex());
  auto& reg = trait_registry();
  if (reg.empty()) return {};
  std::vector<std::string> names;
  names.reserve(reg.size());
  for (const auto& [n, _] : reg) names.push_back(n);
  return names;
}

// Built-in conformance: hard-coded table of which primitive type
// labels conform to which built-in traits. Lets `fn show(x: Stringer)`
// accept Long / String / Array / ... without each builtin needing a
// class-instance wrapper. Only the three built-in traits are
// supported here — user-declared traits still operate solely on
// class instances. Returns false for unknown traits, leaving
// type_matches to fall back to the registered-trait path.
inline bool builtin_conforms_to_trait(std::string_view type_label,
                                       std::string_view trait_name) {
  if (trait_name == "Stringer") {
    // Every value culebra can produce has a `to_string` interpretation
    // via the runtime display path; expose that uniformly.
    return type_label == "Nil" || type_label == "Bool" ||
           type_label == "Long" || type_label == "Float" ||
           type_label == "String" || type_label == "StringView" ||
           type_label == "Array" || type_label == "Tuple" ||
           type_label == "Set" || type_label == "Tensor" ||
           type_label == "Function";
  }
  if (trait_name == "Eq") {
    // Equality is defined on every primitive (value-based) and
    // reference types compare by identity — covers all builtins.
    return type_label == "Nil" || type_label == "Bool" ||
           type_label == "Long" || type_label == "Float" ||
           type_label == "String" || type_label == "StringView" ||
           type_label == "Array" || type_label == "Tuple" ||
           type_label == "Set" || type_label == "Tensor";
  }
  if (trait_name == "Comparable") {
    // Ordering is well-defined on the value primitives. Container
    // types (Array / Tuple / Set / Tensor) do compare lexicographically
    // in the runtime, but we keep this conservative for the MVP.
    return type_label == "Bool" || type_label == "Long" ||
           type_label == "Float" || type_label == "String" ||
           type_label == "StringView";
  }
  if (trait_name == "StringLike") {
    // Byte-readable string flavors: owning `String` and the borrowed
    // `StringView`. User classes that want to be string-substitutable
    // also satisfy this via structural conformance on `to_string_view`.
    return type_label == "String" || type_label == "StringView";
  }
  if (trait_name == "Hashable") {
    // Mirrors what ValueHash / JitValueHash actually hash: every value
    // primitive plus Tuple (hash combines element hashes). Mutable
    // containers (Array / Set / Object / Function / Tensor) stay out
    // — they throw at hash time today and shouldn't pretend otherwise.
    return type_label == "Nil" || type_label == "Bool" ||
           type_label == "Long" || type_label == "Float" ||
           type_label == "String" || type_label == "StringView" ||
           type_label == "Tuple";
  }
  return false;
}

// Structural conformance check: `class_methods` maps method name to
// arity for the class under test. The class conforms when every
// non-default method on `trait` is matched by name and the class
// method accepts at least `trait_arity` positional args. Default
// methods don't need a class-side definition — the trait provides
// them and the dispatcher falls through.
//
// The arity check is one-sided (`class_arity >= trait_arity`) to
// accept class methods that carry extra parameters with defaults
// or a `**kwargs` rest. Strict equality would reject `class C {
// foo(a, **rest) }` against `trait T { foo(a) }`, since the
// class-side walk on the JIT path counts the `**rest` toward
// JitClosure::arity. The runtime call site still verifies actual
// argument counts, so an over-strict class signature surfaces as
// ArityError at call time rather than DispatchError.
inline bool class_conforms_to_trait(
    const std::unordered_map<std::string, size_t>& class_methods,
    const TraitDef& trait) {
  for (const auto& m : trait.methods) {
    if (m.has_default) continue;
    auto it = class_methods.find(m.name);
    if (it == class_methods.end() || it->second < m.arity) return false;
  }
  return true;
}

// Built-in trait declarations evaluated at the top of every program
// (interp + JIT). Provides the standard `Stringer`, `Eq`, and
// `Comparable` abstractions — small enough to inline as a const
// string, structurally usable by any class that ships the right
// methods.
inline std::string_view builtin_traits_preamble() {
  static constexpr std::string_view src = R"culebra(
trait Stringer {
  to_s() -> String
}
trait Eq {
  eq(other) -> Bool
  neq(other) -> Bool { !this.eq(other) }
}
trait Comparable {
  cmp(other) -> Long
  lt(other) -> Bool { this.cmp(other) < 0 }
  le(other) -> Bool { this.cmp(other) <= 0 }
  gt(other) -> Bool { this.cmp(other) > 0 }
  ge(other) -> Bool { this.cmp(other) >= 0 }
}
trait StringLike {
  to_string_view() -> StringView
}
trait Hashable {
  hash() -> Long
}
)culebra";
  return src;
}

}  // namespace culebra
