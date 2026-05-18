#pragma once

// JIT-side implementation of the Culebra standard library.
//
// Independent header. Include from main.cc (or any embedder) after
// jit.h. Provides the runtime functions called from JIT'd code and the
// `JitExtension` struct that fills `JIT::ExtensionHooks`. Embedders
// install the stdlib by calling `culebra::install_jit_stdlib()` once
// before `JIT::run()`.

#include <jit.h>
#include <shared.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>

namespace culebra {
// main.cc (or any embedder) populates this before calling JIT::run.
inline std::vector<std::string>& _culebra_sys_argv_holder() {
  return current_runtime().sys_argv;
}
}  // namespace culebra

using culebra::parse_double_strict;
using culebra::parse_long_strict;
using culebra::throw_type_error_at;

extern "C" {

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_to_long(
    const char* s, int64_t line, int64_t col) {
  if (!s) throw_type_error_at(line, col);
  return parse_long_strict(s, line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_to_long_any(
    int8_t tag, int64_t data, int64_t line, int64_t col) {
  if (tag == TAG_LONG) return {TAG_LONG, data};
  if (tag == TAG_FLOAT) {
    return {TAG_LONG, static_cast<int64_t>(_culebra_float_to_double(data))};
  }
  if (tag == TAG_STRING) {
    return {TAG_LONG,
            parse_long_strict(reinterpret_cast<const char*>(data), line, col)};
  }
  throw_type_error_at(line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_to_float_any(
    int8_t tag, int64_t data, int64_t line, int64_t col) {
  if (tag == TAG_FLOAT) return {TAG_FLOAT, data};
  if (tag == TAG_LONG) {
    return {TAG_FLOAT, _culebra_double_to_bits(static_cast<double>(data))};
  }
  if (tag == TAG_STRING) {
    auto d = parse_double_strict(reinterpret_cast<const char*>(data), line, col);
    return {TAG_FLOAT, _culebra_double_to_bits(d)};
  }
  throw_type_error_at(line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_type_of(int8_t tag) {
  return _culebra_tag_name(tag);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_print(int8_t type,
                                                        int64_t data) {
  if (auto s = _try_str_special(type, data)) {
    std::cout << *s;
    return;
  }
  if (type == 4) {
    std::cout << reinterpret_cast<const char*>(data);
  } else {
    std::cout << _culebra_value_to_str_impl(type, data);
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_input() {
  std::string line;
  if (!std::getline(std::cin, line)) {
    return _culebra_heap_str(std::string(""));
  }
  return _culebra_heap_str(line);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_read_file(
    const char* path, int64_t line, int64_t col) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    throw culebra::CulebraError("IOError",
        std::format("IO.read: cannot open '{}' at {}:{}.", path, line, col),
        line, col);
  }
  std::string s((std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>());
  return _culebra_heap_str(s);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_write_file(
    const char* path, const char* content, int64_t line, int64_t col) {
  std::ofstream ofs(path, std::ios::binary);
  if (!ofs) {
    throw culebra::CulebraError("IOError",
        std::format("IO.write: cannot open '{}' at {}:{}.", path, line, col),
        line, col);
  }
  auto len = std::strlen(content);
  ofs.write(content, static_cast<std::streamsize>(len));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_math_pow(
    int64_t base, int64_t exp, int64_t line, int64_t col) {
  if (exp < 0) culebra::throw_type_error_at(line, col);
  return culebra::ipow_nonneg(base, exp);
}

#define CUL_MATH_F2F(name, call)                                        \
  CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_##name(    \
      int8_t tag, int64_t data, int64_t line, int64_t col) {            \
    double x;                                                           \
    if (tag == TAG_LONG)        x = static_cast<double>(data);          \
    else if (tag == TAG_FLOAT)  x = _culebra_float_to_double(data);     \
    else                        throw_type_error_at(line, col);         \
    return {TAG_FLOAT, _culebra_double_to_bits(call)};                  \
  }
CUL_MATH_F2F(log,  std::log(x))
CUL_MATH_F2F(exp,  std::exp(x))
CUL_MATH_F2F(sqrt, std::sqrt(x))
#undef CUL_MATH_F2F

// Long input is identity (not coerced through Float, which would lose
// precision on values past 2^53).
#define CUL_MATH_F2L(name, call)                                        \
  CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_##name(    \
      int8_t tag, int64_t data, int64_t line, int64_t col) {            \
    if (tag == TAG_LONG) return {TAG_LONG, data};                       \
    if (tag != TAG_FLOAT) throw_type_error_at(line, col);               \
    double x = _culebra_float_to_double(data);                          \
    return {TAG_LONG, static_cast<int64_t>(call)};                      \
  }
CUL_MATH_F2L(floor, std::floor(x))
CUL_MATH_F2L(ceil,  std::ceil(x))
CUL_MATH_F2L(round, std::rint(x))
#undef CUL_MATH_F2L

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_abs(
    int8_t tag, int64_t data, int64_t line, int64_t col) {
  if (tag == TAG_LONG) return {TAG_LONG, data < 0 ? -data : data};
  if (tag == TAG_FLOAT) {
    double x = _culebra_float_to_double(data);
    return {TAG_FLOAT, _culebra_double_to_bits(std::fabs(x))};
  }
  throw_type_error_at(line, col);
}

// Returns Long if every entry was Long, else Float. Callers handle
// retain/release of `args`; the return is numeric (no ref).
inline JitValue _culebra_numeric_reduce(const JitValue* args, int64_t n,
                                        int64_t line, int64_t col,
                                        bool pick_less) {
  if (n < 2) throw_type_error_at(line, col);
  bool any_float = false;
  for (int64_t i = 0; i < n; i++) {
    if (args[i].tag != TAG_LONG && args[i].tag != TAG_FLOAT) {
      throw_type_error_at(line, col);
    }
    if (args[i].tag == TAG_FLOAT) any_float = true;
  }
  if (any_float) {
    double acc = _culebra_coerce_num(args[0].tag, args[0].data);
    for (int64_t i = 1; i < n; i++) {
      double x = _culebra_coerce_num(args[i].tag, args[i].data);
      if (pick_less ? x < acc : x > acc) acc = x;
    }
    return {TAG_FLOAT, _culebra_double_to_bits(acc)};
  }
  int64_t acc = args[0].data;
  for (int64_t i = 1; i < n; i++) {
    int64_t x = args[i].data;
    if (pick_less ? x < acc : x > acc) acc = x;
  }
  return {TAG_LONG, acc};
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_min(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  return _culebra_numeric_reduce(args, n, line, col, /*pick_less=*/true);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_max(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  return _culebra_numeric_reduce(args, n, line, col, /*pick_less=*/false);
}

// --- Random ---

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_random_seed(int64_t n) {
  culebra::random_engine().seed(static_cast<uint64_t>(n));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_random_int(
    int64_t lo, int64_t hi, int64_t line, int64_t col) {
  if (hi <= lo) throw_type_error_at(line, col);
  std::uniform_int_distribution<int64_t> d(lo, hi - 1);
  return d(culebra::random_engine());
}

#define CUL_RANDOM_PAIR(name, Dist)                                     \
  CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_random_##name(  \
      int8_t lt, int64_t ld, int8_t rt, int64_t rd) {                   \
    auto a = _culebra_coerce_num(lt, ld);                               \
    auto b = _culebra_coerce_num(rt, rd);                               \
    Dist d(a, b);                                                       \
    return {TAG_FLOAT,                                                  \
            _culebra_double_to_bits(d(culebra::random_engine()))};      \
  }
CUL_RANDOM_PAIR(uniform, std::uniform_real_distribution<double>)
CUL_RANDOM_PAIR(gauss,   std::normal_distribution<double>)
#undef CUL_RANDOM_PAIR

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_random_shuffle(
    JitArray* arr) {
  std::shuffle(arr->items, arr->items + arr->size, culebra::random_engine());
}

// weighted_choice(pop, weights): returns one element from pop. Pop
// and weights must be equal-length Arrays. Returned value is +1 owned
// by the caller (we retain the picked element before returning).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_random_weighted_choice(
    JitArray* pop, JitArray* weights, int64_t line, int64_t col) {
  if (!pop || !weights || pop->size == 0 ||
      pop->size != weights->size) {
    throw_type_error_at(line, col);
  }
  // Reused between calls to avoid a heap allocation per draw (the
  // microgpt inference path calls this per token).
  thread_local std::vector<double> scratch;
  scratch.clear();
  scratch.reserve(weights->size);
  for (size_t i = 0; i < weights->size; i++) {
    const auto& v = weights->items[i];
    if (v.tag != TAG_LONG && v.tag != TAG_FLOAT) {
      throw_type_error_at(line, col);
    }
    scratch.push_back(_culebra_coerce_num(v.tag, v.data));
  }
  std::discrete_distribution<size_t> d(scratch.begin(), scratch.end());
  auto picked = pop->items[d(culebra::random_engine())];
  culebra_runtime_value_retain(picked.tag, picked.data);
  return picked;
}

// --- IO.exists ---

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_io_exists(
    const char* path) {
  if (!path) return 0;
  std::error_code ec;
  return std::filesystem::exists(path, ec) ? 1 : 0;
}

// --- FS namespace ---

[[noreturn]] inline void _fs_throw_io(const std::string& msg, int64_t line,
                                      int64_t col,
                                      const std::error_code& ec) {
  throw culebra::CulebraError(
      "IOError",
      std::format("{} at {}:{}: {}.", msg, line, col, ec.message()),
      line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_fs_join(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  std::filesystem::path out;
  for (int64_t i = 0; i < n; ++i) {
    if (args[i].tag != TAG_STRING) {
      throw_type_error_at(line, col);
    }
    out /= reinterpret_cast<const char*>(args[i].data);
  }
  return _culebra_heap_str(out.string());
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_fs_basename(
    const char* path) {
  std::filesystem::path p(path ? path : "");
  return _culebra_heap_str(p.filename().string());
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_fs_dirname(
    const char* path) {
  std::filesystem::path p(path ? path : "");
  return _culebra_heap_str(p.parent_path().string());
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_fs_extension(
    const char* path) {
  std::filesystem::path p(path ? path : "");
  return _culebra_heap_str(p.extension().string());
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_fs_stem(
    const char* path) {
  std::filesystem::path p(path ? path : "");
  return _culebra_heap_str(p.stem().string());
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_fs_is_file(
    const char* path) {
  if (!path) return 0;
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) ? 1 : 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_fs_is_dir(
    const char* path) {
  if (!path) return 0;
  std::error_code ec;
  return std::filesystem::is_directory(path, ec) ? 1 : 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_fs_size(
    const char* path, int64_t line, int64_t col) {
  std::error_code ec;
  auto sz = std::filesystem::file_size(path ? path : "", ec);
  if (ec) _fs_throw_io(std::format("FS.size('{}')", path ? path : ""),
                                line, col, ec);
  return static_cast<int64_t>(sz);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_fs_list_dir(
    const char* path, int64_t line, int64_t col) {
  std::error_code ec;
  auto it = std::filesystem::directory_iterator(path ? path : "", ec);
  if (ec) _fs_throw_io(
      std::format("FS.list_dir('{}')", path ? path : ""), line, col, ec);
  auto* arr = culebra_runtime_array_new();
  for (const auto& entry : it) {
    auto* s = _culebra_heap_str(entry.path().filename().string());
    culebra_runtime_array_push(arr, TAG_STRING,
                                reinterpret_cast<int64_t>(s));
  }
  return arr;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_fs_mkdir(
    const char* path, int64_t line, int64_t col) {
  std::error_code ec;
  std::filesystem::create_directories(path ? path : "", ec);
  if (ec) _fs_throw_io(
      std::format("FS.mkdir('{}')", path ? path : ""), line, col, ec);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_fs_remove(
    const char* path, int64_t line, int64_t col) {
  std::error_code ec;
  if (!std::filesystem::remove(path ? path : "", ec) || ec) {
    _fs_throw_io(
        std::format("FS.remove('{}')", path ? path : ""), line, col, ec);
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_sys_exit(int64_t code) {
  std::exit(static_cast<int>(code));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_sys_env(
    const char* name) {
  const char* v = std::getenv(name);
  return _culebra_heap_str(std::string(v ? v : ""));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE double culebra_runtime_sys_time() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  auto now = clock::now();
  return std::chrono::duration<double>(now - t0).count();
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_sys_argv() {
  auto& argv = culebra::_culebra_sys_argv_holder();
  auto* r = culebra_runtime_array_new();
  for (const auto& s : argv) {
    auto* str = _culebra_heap_str(s);
    culebra_runtime_array_push(r, TAG_STRING,
                               reinterpret_cast<int64_t>(str));
  }
  return r;
}

// --- JSON ---

// `indent` > 0 pretty-prints with that many spaces per level; 0 is
// compact. `sort_keys` walks Object keys alphabetically (deterministic
// output). `depth` tracks recursion for indentation only.
inline std::string _jit_json_stringify(int8_t tag, int64_t data,
                                        int indent = 0,
                                        bool sort_keys = false,
                                        int depth = 0) {
  auto sep = [&](int level) -> std::string {
    if (indent <= 0) return "";
    return std::string("\n") + std::string(indent * level, ' ');
  };
  switch (tag) {
    case TAG_NIL:    return "null";
    case TAG_BOOL:   return data ? "true" : "false";
    case TAG_LONG:   return std::to_string(data);
    case TAG_FLOAT: {
      double d = _culebra_float_to_double(data);
      if (!std::isfinite(d)) {
        throw culebra::CulebraError("ValueError",
            "JSON.stringify: non-finite Float");
      }
      return culebra::format_float_shortest(d);
    }
    case TAG_STRING:
      return culebra::json_escape(reinterpret_cast<const char*>(data));
    case TAG_ARRAY:
    case TAG_TUPLE: {
      // Both Array and Tuple share JitArray storage and render as
      // JSON arrays.
      auto* arr = reinterpret_cast<JitArray*>(data);
      if (arr->size == 0) return "[]";
      std::string s = "[";
      for (size_t i = 0; i < arr->size; i++) {
        if (i) s += ",";
        s += sep(depth + 1);
        s += _jit_json_stringify(arr->items[i].tag, arr->items[i].data,
                                 indent, sort_keys, depth + 1);
      }
      s += sep(depth);
      return s + "]";
    }
    case TAG_SET: {
      auto* set = reinterpret_cast<JitSet*>(data);
      if (set->members.empty()) return "[]";
      std::string s = "[";
      for (size_t i = 0; i < set->members.size(); i++) {
        if (i) s += ",";
        s += sep(depth + 1);
        s += _jit_json_stringify(set->members[i].tag,
                                 set->members[i].data, indent,
                                 sort_keys, depth + 1);
      }
      s += sep(depth);
      return s + "]";
    }
    case TAG_OBJECT: {
      auto* obj = reinterpret_cast<JitObject*>(data);
      // Non-String keys are not representable in JSON; reject loudly
      // so round-trip with `JSON.parse` stays consistent.
      if (obj->non_string_props && !obj->non_string_props->empty()) {
        throw culebra::CulebraError("TypeError",
            "JSON.stringify: Object has non-String keys");
      }
      if (!obj->shape || obj->shape->names.empty()) return "{}";
      std::string colon = indent > 0 ? ": " : ":";
      std::vector<size_t> order(obj->shape->names.size());
      for (size_t i = 0; i < order.size(); i++) order[i] = i;
      if (sort_keys) {
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) {
                    return obj->shape->names[a] < obj->shape->names[b];
                  });
      }
      std::string s = "{";
      for (size_t k = 0; k < order.size(); k++) {
        if (k) s += ",";
        s += sep(depth + 1);
        size_t i = order[k];
        s += culebra::json_escape(obj->shape->names[i]) + colon +
             _jit_json_stringify(obj->slots[i].value.tag,
                                 obj->slots[i].value.data, indent,
                                 sort_keys, depth + 1);
      }
      s += sep(depth);
      return s + "}";
    }
  }
  throw culebra::CulebraError("TypeError", std::format(
      "JSON.stringify: cannot serialize {}", _culebra_tag_name(tag)));
}

// RAII guard for a transferred-+1 JitValue. Releases on scope exit
// (normal or throw), so adapter code that takes ownership of a
// positional argument doesn't have to plumb the release through every
// error path. Use `.disarm()` to opt out of the auto-release when the
// value is forwarded into a callee that consumes it.
struct _JitValueGuard {
  int8_t tag;
  int64_t data;
  bool armed = true;
  ~_JitValueGuard() {
    if (armed) _culebra_value_release_impl(tag, data);
  }
  void disarm() { armed = false; }
};

// Splat / explicit kwarg resolver shared between every built-in
// kwarg adapter. The constructor merges splats+kwargs into a single
// name-keyed map (same algorithm as the user-fn runtime resolver:
// splats merge left-to-right with later wins, then explicit kwargs
// layer on top regardless of order). Each map entry owns a +1 ref to
// its value — the destructor drops any leftover entries on every
// exit path, so error throws during typed-take don't leak.
class _JitKwargResolver {
 public:
  _JitKwargResolver(int64_t n_kw, const char* const* kw_keys,
                    JitValue* kw_vals, int64_t n_splat,
                    JitValue* splat_objs, int64_t line, int64_t col,
                    std::string_view ctx)
      : line_(line), col_(col), ctx_(ctx) {
    try {
      build_merged(n_kw, kw_keys, kw_vals, n_splat, splat_objs);
    } catch (...) {
      // build_merged threw mid-loop. The map holds whatever entries
      // we already retained; release them so we don't leak. Caller
      // still owns the un-consumed kwarg / splat slots — those are
      // its responsibility to release on rethrow.
      release_merged();
      throw;
    }
    // Splat operand +1s served their purpose (we retained the values
    // they contained); drop them now.
    for (int64_t i = 0; i < n_splat; i++) {
      _culebra_value_release_impl(splat_objs[i].tag, splat_objs[i].data);
    }
  }
  ~_JitKwargResolver() { release_merged(); }

  _JitKwargResolver(const _JitKwargResolver&) = delete;
  _JitKwargResolver& operator=(const _JitKwargResolver&) = delete;

  // Look up `name` and verify its tag matches `want_tag`. On a tag
  // mismatch raises a TypeError via `fail`. Absent → nullopt.
  std::optional<JitValue> take_typed(std::string_view name, int8_t want_tag,
                                      std::string_view want_name) {
    auto it = merged_.find(name);
    if (it == merged_.end()) return std::nullopt;
    auto v = it->second;
    merged_.erase(it);
    if (v.tag != want_tag) {
      _culebra_value_release_impl(v.tag, v.data);
      fail(std::format("{}: '{}' must be {}", ctx_, name, want_name));
    }
    return v;
  }

  // Throw if any kwargs went unread — i.e. unknown to this built-in.
  void validate_consumed() {
    if (merged_.empty()) return;
    fail(std::format("unknown keyword argument '{}'",
                      merged_.begin()->first));
  }

  // Format and throw a structured CulebraError with " at L:C." in the
  // message (so eval's outer catch doesn't relocate it). Releases the
  // merged map's remaining values before throwing.
  [[noreturn]] void fail(const std::string& msg,
                          std::string_view kind = "TypeError") {
    release_merged();
    throw culebra::CulebraError(std::string(kind),
        std::format("{} at {}:{}.", msg, line_, col_),
        line_, col_);
  }

 private:
  void build_merged(int64_t n_kw, const char* const* kw_keys,
                    JitValue* kw_vals, int64_t n_splat,
                    JitValue* splat_objs) {
    for (int64_t i = 0; i < n_splat; i++) {
      auto sv = splat_objs[i];
      if (sv.tag != TAG_OBJECT) {
        throw culebra::CulebraError("TypeError",
            std::format("**: splat operand must be Object, got {}",
                        _culebra_tag_name(sv.tag)),
            line_, col_);
      }
      auto* obj = reinterpret_cast<JitObject*>(sv.data);
      if (obj->non_string_props && !obj->non_string_props->empty()) {
        throw culebra::CulebraError("TypeError",
            "**: splat key must be String", line_, col_);
      }
      if (!obj->shape) continue;
      for (size_t k = 0; k < obj->shape->names.size(); k++) {
        auto& v = obj->slots[k].value;
        culebra_runtime_value_retain(v.tag, v.data);
        insert_or_replace(obj->shape->names[k], v);
      }
    }
    for (int64_t i = 0; i < n_kw; i++) {
      insert_or_replace(kw_keys[i], kw_vals[i]);
    }
  }
  void insert_or_replace(std::string_view name, JitValue v) {
    auto it = merged_.find(name);
    if (it == merged_.end()) {
      merged_.emplace(name, v);
    } else {
      _culebra_value_release_impl(it->second.tag, it->second.data);
      it->second = v;
    }
  }
  void release_merged() {
    for (auto& [_, v] : merged_) {
      _culebra_value_release_impl(v.tag, v.data);
    }
    merged_.clear();
  }

  std::unordered_map<std::string_view, JitValue> merged_;
  int64_t line_;
  int64_t col_;
  std::string_view ctx_;
};

// Single dispatcher for the JIT side: `lines != 0` switches into the
// JSON-Lines emitter (requires Array/Tuple/Set, rejects indent > 0).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_json_stringify(
    int8_t tag, int64_t data, int64_t indent, int8_t sort_keys,
    int8_t lines) {
  if (!lines) {
    return _culebra_heap_str(
        _jit_json_stringify(tag, data, static_cast<int>(indent),
                            sort_keys != 0, 0));
  }
  if (indent > 0) {
    throw culebra::CulebraError("TypeError",
        "JSON.stringify: lines=true is incompatible with indent>0");
  }
  const JitValue* items = nullptr;
  size_t n = 0;
  if (tag == TAG_ARRAY || tag == TAG_TUPLE) {
    auto* arr = reinterpret_cast<JitArray*>(data);
    items = arr->items;
    n = arr->size;
  } else if (tag == TAG_SET) {
    auto* set = reinterpret_cast<JitSet*>(data);
    items = set->members.data();
    n = set->members.size();
  } else {
    throw culebra::CulebraError("TypeError", std::format(
        "JSON.stringify(lines: true): expects Array/Tuple/Set, got {}",
        _culebra_tag_name(tag)));
  }
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += _jit_json_stringify(items[i].tag, items[i].data, /*indent=*/0,
                             sort_keys != 0, 0);
    s += '\n';
  }
  return _culebra_heap_str(s);
}

// Kwarg adapter for JSON.stringify. Used by the JIT when the call
// carries a dynamic `**splat` operand whose keys can't be enumerated
// at IR-emit time. RAII guards take care of every +1 release path
// (the value, leftover kwargs in the resolver, etc.) so the body
// reads as a flat sequence of takes.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char*
culebra_runtime_json_stringify_kw(
    int8_t v_tag, int64_t v_data,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs,
    int64_t line, int64_t col) {
  _JitValueGuard v_guard{v_tag, v_data};
  _JitKwargResolver kw(n_kw, kw_keys, kw_vals, n_splat, splat_objs,
                       line, col, "JSON.stringify");
  int64_t indent = 0;
  int8_t sort_keys = 0;
  int8_t lines = 0;
  if (auto v = kw.take_typed("indent", TAG_LONG, "Long")) indent = v->data;
  if (auto v = kw.take_typed("sort_keys", TAG_BOOL, "Bool")) {
    sort_keys = v->data ? 1 : 0;
  }
  if (auto v = kw.take_typed("lines", TAG_BOOL, "Bool")) {
    lines = v->data ? 1 : 0;
  }
  kw.validate_consumed();
  return culebra_runtime_json_stringify(v_tag, v_data, indent,
                                         sort_keys, lines);
}

// Minimal recursive-descent JSON parser producing JitValues. Each
// returned heap object (JitArray, JitObject, allocated String) is +1
// owned by the caller, who hands ownership to the user binding.
// Tracks 1-based line/col so failures surface positions via the
// caller's structured Error catch.
struct _JitJsonParser {
  const char* p;
  const char* end;
  long line = 1;
  long col = 1;
  // 'auto' / 'float' — same shape as the interp parser.
  std::string_view number_mode = "auto";

  void advance() {
    if (*p == '\n') { line++; col = 1; }
    else            { col++; }
    ++p;
  }
  void skip_ws() {
    while (p < end &&
           (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
      advance();
    }
  }
  [[noreturn]] void fail(const char* msg) {
    // Embed " at L:C." inline so eval()'s outer catch doesn't override
    // our JSON-internal position with the caller AST's location.
    throw culebra::CulebraError("ValueError",
        std::format("JSON.parse: {} at {}:{}.", msg, line, col),
        line, col);
  }
  JitValue parse_value() {
    skip_ws();
    if (p >= end) fail("unexpected end");
    char c = *p;
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') return parse_string();
    if (c == 't' || c == 'f') return parse_bool();
    if (c == 'n') return parse_null();
    return parse_number();
  }
  JitValue parse_object() {
    advance(); skip_ws();
    auto* obj = culebra_runtime_object_new();
    if (p < end && *p == '}') {
      advance();
      return {TAG_OBJECT, reinterpret_cast<int64_t>(obj)};
    }
    while (p < end) {
      skip_ws();
      auto key = parse_string_raw();
      skip_ws();
      if (p >= end || *p != ':') fail("expected ':'");
      advance();
      auto val = parse_value();
      culebra_runtime_object_set(obj, key.c_str(), false, val.tag, val.data,
                                 0, 0);
      skip_ws();
      if (p < end && *p == ',') { advance(); continue; }
      if (p < end && *p == '}') {
        advance();
        return {TAG_OBJECT, reinterpret_cast<int64_t>(obj)};
      }
      fail("expected ',' or '}'");
    }
    fail("unterminated object");
  }
  JitValue parse_array() {
    advance(); skip_ws();
    auto* arr = culebra_runtime_array_new();
    if (p < end && *p == ']') {
      advance();
      return {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
    }
    while (p < end) {
      auto v = parse_value();
      culebra_runtime_array_push(arr, v.tag, v.data);
      skip_ws();
      if (p < end && *p == ',') { advance(); continue; }
      if (p < end && *p == ']') {
        advance();
        return {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
      }
      fail("expected ',' or ']'");
    }
    fail("unterminated array");
  }
  JitValue parse_string() {
    auto raw = parse_string_raw();
    return {TAG_STRING,
            reinterpret_cast<int64_t>(_culebra_heap_str(raw))};
  }
  std::string parse_string_raw() {
    if (*p != '"') fail("expected string");
    advance();
    std::string out;
    while (p < end && *p != '"') {
      if (*p == '\\' && p + 1 < end) {
        advance();
        switch (*p) {
          case '"':  out += '"'; break;
          case '\\': out += '\\'; break;
          case '/':  out += '/'; break;
          case 'n':  out += '\n'; break;
          case 'r':  out += '\r'; break;
          case 't':  out += '\t'; break;
          case 'b':  out += '\b'; break;
          case 'f':  out += '\f'; break;
          default:   fail("bad escape");
        }
        advance();
      } else {
        out += *p; advance();
      }
    }
    if (p >= end) fail("unterminated string");
    advance();
    return out;
  }
  JitValue parse_bool() {
    if (end - p >= 4 && std::string_view(p, 4) == "true") {
      for (int i = 0; i < 4; i++) advance();
      return {TAG_BOOL, 1};
    }
    if (end - p >= 5 && std::string_view(p, 5) == "false") {
      for (int i = 0; i < 5; i++) advance();
      return {TAG_BOOL, 0};
    }
    fail("bad bool");
  }
  JitValue parse_null() {
    if (end - p >= 4 && std::string_view(p, 4) == "null") {
      for (int i = 0; i < 4; i++) advance();
      return {TAG_NIL, 0};
    }
    fail("bad null");
  }
  JitValue parse_number() {
    const char* start = p;
    if (*p == '-') advance();
    bool is_float = false;
    const char* digits_start = p;
    while (p < end && (*p >= '0' && *p <= '9')) advance();
    if (p == digits_start) fail("expected value");
    if (p < end && *p == '.') {
      is_float = true; advance();
      while (p < end && (*p >= '0' && *p <= '9')) advance();
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
      is_float = true; advance();
      if (p < end && (*p == '+' || *p == '-')) advance();
      while (p < end && (*p >= '0' && *p <= '9')) advance();
    }
    std::string buf(start, p);
    if (number_mode == "float") {
      return {TAG_FLOAT, _culebra_double_to_bits(std::stod(buf))};
    }
    if (is_float) {
      return {TAG_FLOAT, _culebra_double_to_bits(std::stod(buf))};
    }
    return {TAG_LONG, static_cast<int64_t>(std::stoll(buf))};
  }
};

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_json_parse(
    const char* s, const char* number_mode, int8_t lines) {
  if (!lines) {
    _JitJsonParser jp{s, s + std::strlen(s)};
    jp.number_mode = number_mode;
    auto v = jp.parse_value();
    jp.skip_ws();
    if (jp.p != jp.end) {
      throw culebra::CulebraError("ValueError",
          std::format("JSON.parse: trailing characters at {}:{}.",
                      jp.line, jp.col),
          jp.line, jp.col);
    }
    return v;
  }
  // JSON Lines: split on `\n`, parse each non-empty slice; return an
  // Array. The line counter advances with each `\n` so error positions
  // stay coherent across the whole input.
  auto* arr = culebra_runtime_array_new();
  long lineno = 1;
  size_t n = std::strlen(s);
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j < n && s[j] != '\n') j++;
    if (j > i) {
      _JitJsonParser jp{s + i, s + j};
      jp.line = lineno;
      jp.number_mode = number_mode;
      auto v = jp.parse_value();
      jp.skip_ws();
      if (jp.p != jp.end) {
        culebra_runtime_value_release(TAG_ARRAY,
                                      reinterpret_cast<int64_t>(arr));
        throw culebra::CulebraError("ValueError",
            std::format("JSON.parse(lines: true): trailing characters "
                        "at {}:{}.", jp.line, jp.col),
            jp.line, jp.col);
      }
      culebra_runtime_array_push(arr, v.tag, v.data);
    }
    if (j == n) break;
    i = j + 1;
    lineno++;
  }
  return {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
}

// Kwarg adapter for JSON.parse. `s` is a non-refcounted TAG_STRING
// cstring — no value guard needed. Everything else (splat values,
// merged map) is handled by `_JitKwargResolver`.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_json_parse_kw(
    const char* s,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs,
    int64_t line, int64_t col) {
  _JitKwargResolver kw(n_kw, kw_keys, kw_vals, n_splat, splat_objs,
                       line, col, "JSON.parse");
  const char* number_mode = "auto";
  int8_t lines = 0;
  if (auto v = kw.take_typed("number_mode", TAG_STRING, "String")) {
    number_mode = reinterpret_cast<const char*>(v->data);
  }
  if (auto v = kw.take_typed("lines", TAG_BOOL, "Bool")) {
    lines = v->data ? 1 : 0;
  }
  kw.validate_consumed();
  return culebra_runtime_json_parse(s, number_mode, lines);
}

}  // extern "C"

// --- JIT compile-side dispatch (extension hook implementation) ---

namespace culebra {

// JitExtension fills the JIT::ExtensionHooks for the standard library
// (Math/IO/Random/Sys + bare globals like puts/to_long/type_of and the
// `Math.range(N).<HOF>(...)` fusion peephole). Declared as a friend of
// JIT in jit.h so each member can reach the JIT internals it needs
// (builder_/module_/valueType_/make_*/extract_*/...) without the JIT
// class having to expose them publicly.
struct JitExtension {
  static void declare_runtime(JIT& jit);
  static llvm::Value* compile_global(JIT& jit, const std::string& name,
                                       const peg::Ast& argsAst,
                                       const peg::Ast& callAst);
  static llvm::Value* compile_ns_call(JIT& jit,
                                        std::string_view ns,
                                        std::string_view method,
                                        const peg::Ast& argsAst,
                                        const peg::Ast& callAst);
  static llvm::Value* compile_ns_prop(JIT& jit,
                                        std::string_view ns,
                                        std::string_view prop);

  // Dynamic `**variable` splat path for JSON.{stringify, parse}.
  // Materializes the ARG_LIST as slabs (positional[0], kwarg keys+vals,
  // splat objects) and hands them to the runtime adapter so splat
  // enumeration happens at runtime.
  static llvm::Value* compile_json_kwargs_adapter(
      JIT& jit, std::string_view method, const peg::Ast& argsAst);

  static llvm::Value* emit_output_call(JIT& jit, const char* rt_name,
                                         const peg::Ast& argsAst);

  static bool is_builtin_var(const std::string& name);
  static llvm::Value* compile_ufcs_builtin(JIT& jit,
                                             const std::string& method,
                                             const peg::Ast& argsAst,
                                             llvm::Value* receiver);

  static void install() {
    JIT::install_extension({
        .declare_runtime = &declare_runtime,
        .compile_global = &compile_global,
        .compile_ns_call = &compile_ns_call,
        .compile_ns_prop = &compile_ns_prop,
        .is_builtin_var = &is_builtin_var,
        .compile_ufcs_builtin = &compile_ufcs_builtin,
    });
  }
};

// Convenience wrapper for embedders. Call once before JIT::run().
inline void install_jit_stdlib() { JitExtension::install(); }

inline void JitExtension::declare_runtime(JIT& jit) {
  auto ptrTy = llvm::PointerType::get(jit.ctx_, 0);

  jit.module_->getOrInsertFunction(rt::print, jit.builder_.getVoidTy(),
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::to_long,
                               jit.builder_.getInt64Ty(), ptrTy,
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::to_long_any, jit.valueType_,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::to_float_any, jit.valueType_,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::type_of, ptrTy,
                               jit.builder_.getInt8Ty());
  jit.module_->getOrInsertFunction(rt::input, ptrTy);
  jit.module_->getOrInsertFunction(rt::read_file, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::write_file,
                               jit.builder_.getVoidTy(), ptrTy, ptrTy,
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::math_pow,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  // Float-domain Math: (tag, data, line, col) -> JitValue. Register
  // the full family in one pass since they share the same signature.
  for (auto name : {rt::math_log, rt::math_exp, rt::math_sqrt,
                    rt::math_floor, rt::math_ceil, rt::math_round,
                    rt::math_abs}) {
    jit.module_->getOrInsertFunction(name, jit.valueType_,
                                 jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty());
  }
  // Variadic min/max: (args_ptr, n, line, col) -> JitValue.
  for (auto name : {rt::math_min, rt::math_max}) {
    jit.module_->getOrInsertFunction(name, jit.valueType_, ptrTy,
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty());
  }
  jit.module_->getOrInsertFunction(rt::sys_exit, jit.builder_.getVoidTy(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::sys_env, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::sys_argv, ptrTy);
  jit.module_->getOrInsertFunction(rt::sys_time, jit.builder_.getDoubleTy());
  // Random
  jit.module_->getOrInsertFunction(rt::random_seed, jit.builder_.getVoidTy(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::random_int, jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::random_uniform, jit.valueType_,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::random_gauss, jit.valueType_,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::random_shuffle, jit.builder_.getVoidTy(),
                               ptrTy);
  jit.module_->getOrInsertFunction(rt::random_weighted_choice, jit.valueType_,
                               ptrTy, ptrTy, jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::io_exists, jit.builder_.getInt64Ty(), ptrTy);
  // FS: (path) -> string for path manipulators, (path) -> i64 for queries,
  // (path, line, col) -> i64/void/ptr for ops that throw on error.
  jit.module_->getOrInsertFunction(rt::fs_join, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  for (auto name : {rt::fs_basename, rt::fs_dirname, rt::fs_extension,
                    rt::fs_stem}) {
    jit.module_->getOrInsertFunction(name, ptrTy, ptrTy);
  }
  for (auto name : {rt::fs_is_file, rt::fs_is_dir}) {
    jit.module_->getOrInsertFunction(name, jit.builder_.getInt64Ty(), ptrTy);
  }
  jit.module_->getOrInsertFunction(rt::fs_size,
                               jit.builder_.getInt64Ty(), ptrTy,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::fs_list_dir, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  for (auto name : {rt::fs_mkdir, rt::fs_remove}) {
    jit.module_->getOrInsertFunction(name, jit.builder_.getVoidTy(), ptrTy,
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty());
  }

  // Tensor: zeros/ones/randn use the variadic (args_ptr, n, line, col)
  // -> ptr signature; from takes a single Array (ptr, line, col);
  // shape is just (Tensor*) -> Array*.
  for (auto name : {rt::tensor_zeros, rt::tensor_ones, rt::tensor_randn}) {
    jit.module_->getOrInsertFunction(name, ptrTy, ptrTy,
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty());
  }
  jit.module_->getOrInsertFunction(rt::tensor_from, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_shape, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_eval_one,
                               jit.builder_.getVoidTy(), ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_binop, ptrTy,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_transpose, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_clone, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_slice, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_reshape, ptrTy, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_reduce_axis, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_reduce_all, jit.valueType_,
                               ptrTy, jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_to_array, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_dot, ptrTy, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_from_csv, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_unary, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_linear_sigmoid, ptrTy,
                               ptrTy, ptrTy, ptrTy);

  // Iterator terminal methods.
  auto i64 = jit.builder_.getInt64Ty();
  auto i8 = jit.builder_.getInt8Ty();
  jit.module_->getOrInsertFunction(rt::iter_collect, ptrTy, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_count, i64, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_for_each, jit.builder_.getVoidTy(),
                               i8, i64, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_reduce, jit.builder_.getVoidTy(),
                               i8, i64, i8, i64, i8, i64, i64, i64,
                               ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::iter_find, jit.builder_.getVoidTy(),
                               i8, i64, i8, i64, i64, i64, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::iter_any, i64, i8, i64, i8, i64, i64,
                               i64);
  // sum/product/min/max: (it, id, line, col) -> i64
  jit.module_->getOrInsertFunction(rt::iter_sum, i64, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_product, i64, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_min, i64, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_max, i64, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_all, i64, i8, i64, i8, i64, i64,
                               i64);

  // Iterator lazy factories.
  jit.module_->getOrInsertFunction(rt::iter_map, ptrTy, i8, i64, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_filter, ptrTy, i8, i64, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_take, ptrTy, i8, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_skip, ptrTy, i8, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_take_while, ptrTy, i8, i64, i8, i64);
  // chain/zip/flat_map carry line+col for the "not iterable" error.
  jit.module_->getOrInsertFunction(rt::iter_chain, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_zip, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_enumerate, ptrTy, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_flat_map, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  // (next_cls, iter_tag, iter_data, &out_tag, &out_data) -> i64 (1/0)
  jit.module_->getOrInsertFunction(rt::iter_advance, i64, ptrTy, i8, i64, ptrTy,
                               ptrTy);
  jit.module_->getOrInsertFunction(rt::str_code_points, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::str_graphemes, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::array_iter, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::object_iter, ptrTy, ptrTy);

  // JSON: stringify takes (tag, data, indent, sort_keys, lines) ->
  // String; parse takes (String, number_mode_cstr, lines) -> Value.
  jit.module_->getOrInsertFunction(rt::json_stringify, ptrTy,
                                jit.builder_.getInt8Ty(),
                                jit.builder_.getInt64Ty(),
                                jit.builder_.getInt64Ty(),
                                jit.builder_.getInt8Ty(),
                                jit.builder_.getInt8Ty());
  jit.module_->getOrInsertFunction(rt::json_parse, jit.valueType_, ptrTy,
                                ptrTy, jit.builder_.getInt8Ty());
  // Kwarg adapters: route dynamic `**splat` calls through these so
  // splat enumeration happens at runtime. See
  // `culebra_runtime_json_stringify_kw` / `_parse_kw`.
  jit.module_->getOrInsertFunction(
      rt::json_stringify_kw, ptrTy,
      jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
      jit.builder_.getInt64Ty(), ptrTy, ptrTy,
      jit.builder_.getInt64Ty(), ptrTy,
      jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(
      rt::json_parse_kw, jit.valueType_, ptrTy,
      jit.builder_.getInt64Ty(), ptrTy, ptrTy,
      jit.builder_.getInt64Ty(), ptrTy,
      jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
}

inline llvm::Value* JitExtension::compile_global(JIT& jit,
                                                  const std::string& name,
                                                  const peg::Ast& argsAst,
                                                  const peg::Ast& callAst) {
  // Global builtins (puts, assert, etc.) parse positionally. None of
  // them accept kwargs today; if the call carries kwargs/splats and
  // matches a built-in name surface a clean SyntaxError. If the user
  // shadowed the name with their own `let X = fn (...) {...}` binding,
  // defer to the downstream user-fn resolver (mirrors interp's
  // name-resolution-first behavior). Otherwise return nullptr so
  // downstream call dispatch can take over.
  if (!JIT::arg_list_is_positional_only(argsAst)) {
    if (jit.lookup_fn_ast(name) != nullptr) return nullptr;
    static const std::set<std::string_view> known_globals = {
        "puts", "print", "assert", "to_long", "to_float",
        "to_string", "type_of", "iota", "range",
    };
    if (known_globals.contains(name)) {
      throw culebra::CulebraError("SyntaxError",
          std::format("'{}' does not accept keyword arguments", name),
          argsAst.line, argsAst.column);
    }
    return nullptr;
  }
  auto line = jit.builder_.getInt64(callAst.line);
  auto col = jit.builder_.getInt64(callAst.column);

  if (name == "puts" && argsAst.nodes.size() == 1)
    return emit_output_call(jit, rt::puts, argsAst);
  if (name == "print" && argsAst.nodes.size() == 1)
    return emit_output_call(jit, rt::print, argsAst);

  if (name == "assert" && argsAst.nodes.size() == 1) {
    auto arg = jit.compile(*argsAst.nodes[0]);
    jit.emit_call(
        jit.module_->getFunction(rt::assert_),
        {jit.extract_tag(arg), jit.extract_data(arg), line, col});
    jit.emit_value_release(arg);
    return jit.make_nil();
  }

  if (name == "to_long" && argsAst.nodes.size() == 1) {
    auto arg = jit.compile(*argsAst.nodes[0]);
    // Polymorphic: Long/Float/String. The runtime helper dispatches
    // on tag (Long identity, Float truncate, String parse) and
    // raises `type error` for anything else.
    auto result = jit.emit_call(
        jit.module_->getFunction(rt::to_long_any),
        {jit.extract_tag(arg), jit.extract_data(arg), line, col});
    jit.emit_value_release(arg);
    return result;
  }

  if (name == "to_float" && argsAst.nodes.size() == 1) {
    auto arg = jit.compile(*argsAst.nodes[0]);
    auto result = jit.emit_call(
        jit.module_->getFunction(rt::to_float_any),
        {jit.extract_tag(arg), jit.extract_data(arg), line, col});
    jit.emit_value_release(arg);
    return result;
  }

  if (name == "to_string" && argsAst.nodes.size() == 1) {
    auto arg = jit.compile(*argsAst.nodes[0]);
    auto s = jit.emit_call(
        jit.module_->getFunction(rt::value_to_display),
        {jit.extract_tag(arg), jit.extract_data(arg)});
    jit.emit_value_release(arg);
    return jit.make_string(s);
  }

  if (name == "type_of" && argsAst.nodes.size() == 1) {
    auto arg = jit.compile(*argsAst.nodes[0]);
    auto s = jit.emit_call(jit.module_->getFunction(rt::type_of),
                                 {jit.extract_tag(arg)});
    jit.emit_value_release(arg);
    return jit.make_string(s);
  }

  return nullptr;
}

// Member-style shorthands so each JitExtension::compile_* body can call
// `make_long(v)` / `extract_tag(v)` / `module_->...` directly instead
// of `jit.make_long(v)` / `jit.module_->...`. References for true JIT
// members; forwarding lambdas for member functions (C++ has no member
// aliasing). Undefined at the end of this header.
#define CULEBRA_JIT_EXT_BODY_ALIASES(jit)                                     \
  auto& module_ = jit.module_;                                                \
  auto& builder_ = jit.builder_;                                              \
  auto& valueType_ = jit.valueType_;                                          \
  auto& ctx_ = jit.ctx_;                                                      \
  auto extract_tag = [&](llvm::Value* v) { return jit.extract_tag(v); };      \
  auto extract_data = [&](llvm::Value* v) { return jit.extract_data(v); };    \
  auto make_long = [&](llvm::Value* v) { return jit.make_long(v); };          \
  auto make_string = [&](llvm::Value* v) { return jit.make_string(v); };      \
  auto make_array = [&](llvm::Value* v) { return jit.make_array(v); };        \
  auto make_object = [&](llvm::Value* v) { return jit.make_object(v); };      \
  auto make_tensor = [&](llvm::Value* v) { return jit.make_tensor(v); };      \
  auto make_nil = [&]() { return jit.make_nil(); };                           \
  auto make_bool = [&](llvm::Value* v) { return jit.make_bool(v); };          \
  auto make_float = [&](llvm::Value* v) { return jit.make_float(v); };        \
  auto value_to_long = [&](llvm::Value* v) { return jit.value_to_long(v); };  \
  auto emit_value_release = [&](llvm::Value* v) {                             \
    return jit.emit_value_release(v);                                         \
  };                                                                          \
  auto emit_type_check = [&](llvm::Value* v, std::string_view t,              \
                             const char* w) {                                 \
    return jit.emit_type_check(v, t, w);                                      \
  };                                                                          \
  auto compile = [&](const peg::Ast& a) { return jit.compile(a); };           \
  auto emit_output_call = [&](const char* rt, const peg::Ast& a) {            \
    return JitExtension::emit_output_call(jit, rt, a);                        \
  };                                                                          \
  auto emit_call = [&](llvm::FunctionCallee callee,                           \
                       llvm::ArrayRef<llvm::Value*> args) {                   \
    return jit.emit_call(callee, args);                                       \
  }

inline llvm::Value* JitExtension::compile_ns_call(JIT& jit,
                                                    std::string_view ns,
                                                    std::string_view method,
                                                    const peg::Ast& argsAst,
                                                    const peg::Ast& callAst) {
  CULEBRA_JIT_EXT_BODY_ALIASES(jit);
  // JSON.{stringify, parse} accept kwargs (indent / sort_keys / lines /
  // number_mode) and parse the ARG_LIST themselves. Other registered
  // namespaces (Math/IO/Random/Sys) parse positionally and don't
  // accept kwargs. For an IDENTIFIER.method() call whose `ns` is NOT
  // one of these, fall through to nullptr so the regular dispatch
  // (compile_call → compile_method_call → user-method / UFCS) can
  // handle it — that path supports kwargs against user closures.
  if (!JIT::arg_list_is_positional_only(argsAst)) {
    static const std::set<std::string_view> kwarg_aware_ns = {"JSON"};
    static const std::set<std::string_view> positional_only_ns = {
        "Math", "IO", "FS", "Random", "Sys", "Tensor",
    };
    if (positional_only_ns.contains(ns)) {
      throw culebra::CulebraError("SyntaxError",
          std::format("namespace '{}' does not accept keyword arguments",
                      ns),
          argsAst.line, argsAst.column);
    }
    if (!kwarg_aware_ns.contains(ns)) {
      return nullptr;  // unknown namespace → let compile_call dispatch
    }
  }
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto line = builder_.getInt64(callAst.line);
  auto col = builder_.getInt64(callAst.column);

  if (ns == "Math") {
    auto build_block = [&](const char* tag) {
      return llvm::BasicBlock::Create(
          ctx_, tag, builder_.GetInsertBlock()->getParent());
    };

    // Single-arg helper with a Long fast path. If the argument is
    // TAG_LONG at runtime, run `long_fast` inline on its i64 payload
    // (abs: neg-or-self; floor/ceil/round: identity). Otherwise fall
    // back to the runtime helper, which handles Float and raises
    // `type error` on non-numeric input.
    auto math_unary = [&](const char* rt_name,
                          auto long_fast) -> llvm::Value* {
      if (argsAst.nodes.size() != 1) return nullptr;
      auto arg = compile(*argsAst.nodes[0]);
      auto tag = extract_tag(arg);
      auto data = extract_data(arg);
      auto intBB = build_block("math.int");
      auto slowBB = build_block("math.slow");
      auto mergeBB = build_block("math.merge");
      auto isLong =
          builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_LONG));
      builder_.CreateCondBr(isLong, intBB, slowBB);

      builder_.SetInsertPoint(intBB);
      auto intResult = long_fast(data);
      auto intEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(slowBB);
      auto slowResult = emit_call(
          module_->getFunction(rt_name), {tag, data, line, col});
      auto slowEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(mergeBB);
      auto phi = builder_.CreatePHI(valueType_, 2, "math.r");
      phi->addIncoming(intResult, intEndBB);
      phi->addIncoming(slowResult, slowEndBB);
      emit_value_release(arg);
      return static_cast<llvm::Value*>(phi);
    };

    // Single-arg helper without a Long fast path: log/exp/sqrt return
    // Float for every numeric input, so there's no benefit to
    // branching (the runtime's SIToFP happens regardless).
    auto math_unary_runtime = [&](const char* rt_name) -> llvm::Value* {
      if (argsAst.nodes.size() != 1) return nullptr;
      auto arg = compile(*argsAst.nodes[0]);
      auto result = emit_call(
          module_->getFunction(rt_name),
          {extract_tag(arg), extract_data(arg), line, col});
      emit_value_release(arg);
      return result;
    };

    if (method == "abs") {
      if (auto v = math_unary(rt::math_abs, [&](llvm::Value* d) {
            auto isNeg = builder_.CreateICmpSLT(d, builder_.getInt64(0));
            return make_long(
                builder_.CreateSelect(isNeg, builder_.CreateNeg(d), d));
          })) return v;
    }
    // For floor/ceil/round on Long, the result is the input unchanged.
    auto long_identity = [&](llvm::Value* d) { return make_long(d); };
    if (method == "floor") { if (auto v = math_unary(rt::math_floor, long_identity)) return v; }
    if (method == "ceil")  { if (auto v = math_unary(rt::math_ceil,  long_identity)) return v; }
    if (method == "round") { if (auto v = math_unary(rt::math_round, long_identity)) return v; }
    if (method == "log")   { if (auto v = math_unary_runtime(rt::math_log))   return v; }
    if (method == "exp")   { if (auto v = math_unary_runtime(rt::math_exp))   return v; }
    if (method == "sqrt")  { if (auto v = math_unary_runtime(rt::math_sqrt))  return v; }

    // min/max: compile every arg, stash them in a stack array of
    // JitValue, and call the variadic helper with (&args[0], n).
    // 2-arg fast path: if both operands are TAG_LONG, use an inline
    // icmp+select — avoids the alloca+runtime call for the common
    // Math.min(a, b) / Math.max(a, b) shape. Any tag mismatch
    // (including Float) falls through to the variadic helper which
    // handles promotion and error reporting.
    auto reduce_pair = [&](llvm::Value* lhs, llvm::Value* rhs,
                           const char* rt_name,
                           llvm::CmpInst::Predicate pred) {
      auto ltag = extract_tag(lhs);
      auto rtag = extract_tag(rhs);
      auto longTag = builder_.getInt8(TAG_LONG);
      auto lIsLong = builder_.CreateICmpEQ(ltag, longTag);
      auto rIsLong = builder_.CreateICmpEQ(rtag, longTag);
      auto bothLong = builder_.CreateAnd(lIsLong, rIsLong, "both.long");
      auto intBB = build_block("reduce.int");
      auto slowBB = build_block("reduce.slow");
      auto mergeBB = build_block("reduce.merge");
      builder_.CreateCondBr(bothLong, intBB, slowBB);

      builder_.SetInsertPoint(intBB);
      auto ldata = extract_data(lhs);
      auto rdata = extract_data(rhs);
      auto cmp = builder_.CreateICmp(pred, ldata, rdata);
      auto intResult = make_long(builder_.CreateSelect(cmp, ldata, rdata));
      auto intEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(slowBB);
      llvm::IRBuilder<> entryB(
          &builder_.GetInsertBlock()->getParent()->getEntryBlock(),
          builder_.GetInsertBlock()->getParent()->getEntryBlock().begin());
      auto argsAlloca = entryB.CreateAlloca(valueType_,
                                            builder_.getInt64(2),
                                            "num.args2");
      builder_.CreateStore(lhs, argsAlloca);
      auto slot1 = builder_.CreateGEP(valueType_, argsAlloca,
                                      builder_.getInt64(1));
      builder_.CreateStore(rhs, slot1);
      auto slowResult = emit_call(
          module_->getFunction(rt_name),
          {argsAlloca, builder_.getInt64(2), line, col});
      auto slowEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(mergeBB);
      auto phi = builder_.CreatePHI(valueType_, 2, "reduce.r");
      phi->addIncoming(intResult, intEndBB);
      phi->addIncoming(slowResult, slowEndBB);
      return static_cast<llvm::Value*>(phi);
    };

    auto variadic_reduce = [&](const char* rt_name,
                               llvm::CmpInst::Predicate pred) -> llvm::Value* {
      auto n = argsAst.nodes.size();
      if (n < 2) return nullptr;
      if (n == 2) {
        auto a = compile(*argsAst.nodes[0]);
        auto b = compile(*argsAst.nodes[1]);
        auto r = reduce_pair(a, b, rt_name, pred);
        emit_value_release(a);
        emit_value_release(b);
        return r;
      }
      llvm::IRBuilder<> entryB(
          &builder_.GetInsertBlock()->getParent()->getEntryBlock(),
          builder_.GetInsertBlock()->getParent()->getEntryBlock().begin());
      auto argsAlloca = entryB.CreateAlloca(
          valueType_, builder_.getInt64(static_cast<int64_t>(n)),
          "num.args");
      std::vector<llvm::Value*> compiled;
      compiled.reserve(n);
      for (size_t i = 0; i < n; i++) {
        auto v = compile(*argsAst.nodes[i]);
        auto slot = builder_.CreateGEP(valueType_, argsAlloca,
                                       builder_.getInt64(i));
        builder_.CreateStore(v, slot);
        compiled.push_back(v);
      }
      auto result = emit_call(
          module_->getFunction(rt_name),
          {argsAlloca, builder_.getInt64(static_cast<int64_t>(n)),
           line, col});
      for (auto v : compiled) emit_value_release(v);
      return result;
    };
    if (method == "min") {
      if (auto v = variadic_reduce(rt::math_min,
                                   llvm::CmpInst::ICMP_SLT)) return v;
    }
    if (method == "max") {
      if (auto v = variadic_reduce(rt::math_max,
                                   llvm::CmpInst::ICMP_SGT)) return v;
    }

    if (method == "pow" && argsAst.nodes.size() == 2) {
      auto base = value_to_long(compile(*argsAst.nodes[0]));
      auto exp = value_to_long(compile(*argsAst.nodes[1]));
      auto r = emit_call(module_->getFunction(rt::math_pow),
                                   {base, exp, line, col});
      return make_long(r);
    }
    if (method == "sign" && argsAst.nodes.size() == 1) {
      auto x = value_to_long(compile(*argsAst.nodes[0]));
      auto zero = builder_.getInt64(0);
      auto is_neg = builder_.CreateICmpSLT(x, zero);
      auto is_pos = builder_.CreateICmpSGT(x, zero);
      auto pos_or_zero = builder_.CreateSelect(is_pos, builder_.getInt64(1),
                                               zero);
      auto r = builder_.CreateSelect(is_neg, builder_.getInt64(-1),
                                     pos_or_zero);
      return make_long(r);
    }
    if (method == "clamp" && argsAst.nodes.size() == 3) {
      auto x = value_to_long(compile(*argsAst.nodes[0]));
      auto lo = value_to_long(compile(*argsAst.nodes[1]));
      auto hi = value_to_long(compile(*argsAst.nodes[2]));
      auto above_lo = builder_.CreateSelect(builder_.CreateICmpSLT(x, lo),
                                            lo, x);
      auto r = builder_.CreateSelect(builder_.CreateICmpSGT(above_lo, hi),
                                     hi, above_lo);
      return make_long(r);
    }
  }

  if (ns == "IO") {
    if (method == "puts" && argsAst.nodes.size() == 1)
      return emit_output_call(rt::puts, argsAst);
    if (method == "print" && argsAst.nodes.size() == 1)
      return emit_output_call(rt::print, argsAst);
    if (method == "input" && argsAst.nodes.size() == 0) {
      auto s = emit_call(module_->getFunction(rt::input), {});
      return make_string(s);
    }
    if (method == "read" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "IO.read argument");
      auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto s = emit_call(
          module_->getFunction(rt::read_file), {ptr, line, col});
      emit_value_release(arg);
      return make_string(s);
    }
    if (method == "write" && argsAst.nodes.size() == 2) {
      auto p = compile(*argsAst.nodes[0]);
      emit_type_check(p, "String", "IO.write path");
      auto c = compile(*argsAst.nodes[1]);
      emit_type_check(c, "String", "IO.write content");
      auto pp = builder_.CreateIntToPtr(extract_data(p), ptrTy);
      auto cp = builder_.CreateIntToPtr(extract_data(c), ptrTy);
      emit_call(module_->getFunction(rt::write_file),
                          {pp, cp, line, col});
      emit_value_release(p);
      emit_value_release(c);
      return make_nil();
    }
    if (method == "exists" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "IO.exists argument");
      auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto i = emit_call(module_->getFunction(rt::io_exists),
                                   {ptr});
      emit_value_release(arg);
      auto b = builder_.CreateICmpNE(i, builder_.getInt64(0));
      return make_bool(b);
    }
  }

  if (ns == "FS") {
    // (path) -> String for the four path-manipulation helpers.
    auto path_to_string = [&](const char* rt_name) -> llvm::Value* {
      if (argsAst.nodes.size() != 1) return nullptr;
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "FS path argument");
      auto p = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto s = emit_call(module_->getFunction(rt_name), {p});
      emit_value_release(arg);
      return make_string(s);
    };
    if (method == "basename")  if (auto v = path_to_string(rt::fs_basename)) return v;
    if (method == "dirname")   if (auto v = path_to_string(rt::fs_dirname)) return v;
    if (method == "extension") if (auto v = path_to_string(rt::fs_extension)) return v;
    if (method == "stem")      if (auto v = path_to_string(rt::fs_stem)) return v;

    // (path) -> Bool query helpers.
    auto path_to_bool = [&](const char* rt_name) -> llvm::Value* {
      if (argsAst.nodes.size() != 1) return nullptr;
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "FS path argument");
      auto p = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto i = emit_call(module_->getFunction(rt_name), {p});
      emit_value_release(arg);
      auto b = builder_.CreateICmpNE(i, builder_.getInt64(0));
      return make_bool(b);
    };
    if (method == "exists")  if (auto v = path_to_bool(rt::io_exists)) return v;
    if (method == "is_file") if (auto v = path_to_bool(rt::fs_is_file)) return v;
    if (method == "is_dir")  if (auto v = path_to_bool(rt::fs_is_dir)) return v;

    // (path, line, col) -> Long. IOError on missing.
    if (method == "size" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "FS.size argument");
      auto p = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto n = emit_call(module_->getFunction(rt::fs_size),
                         {p, line, col});
      emit_value_release(arg);
      return make_long(n);
    }

    // (path, line, col) -> Array*.
    if (method == "list_dir" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "FS.list_dir argument");
      auto p = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto a = emit_call(module_->getFunction(rt::fs_list_dir),
                         {p, line, col});
      emit_value_release(arg);
      return make_array(a);
    }

    // (path, line, col) -> void mutators.
    auto path_to_void = [&](const char* rt_name) -> llvm::Value* {
      if (argsAst.nodes.size() != 1) return nullptr;
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "FS path argument");
      auto p = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      emit_call(module_->getFunction(rt_name), {p, line, col});
      emit_value_release(arg);
      return make_nil();
    };
    if (method == "mkdir")  if (auto v = path_to_void(rt::fs_mkdir)) return v;
    if (method == "remove") if (auto v = path_to_void(rt::fs_remove)) return v;

    // (args*) -> String join; build a slab of JitValues then call.
    if (method == "join") {
      auto n = argsAst.nodes.size();
      llvm::IRBuilder<> entryB(
          &builder_.GetInsertBlock()->getParent()->getEntryBlock(),
          builder_.GetInsertBlock()->getParent()->getEntryBlock().begin());
      auto slab = entryB.CreateAlloca(
          valueType_, builder_.getInt64(static_cast<int64_t>(n)),
          "fs.join.args");
      std::vector<llvm::Value*> compiled;
      compiled.reserve(n);
      for (size_t i = 0; i < n; i++) {
        auto v = compile(*argsAst.nodes[i]);
        auto slot = builder_.CreateGEP(valueType_, slab,
                                       builder_.getInt64(i));
        builder_.CreateStore(v, slot);
        compiled.push_back(v);
      }
      auto s = emit_call(
          module_->getFunction(rt::fs_join),
          {slab, builder_.getInt64(static_cast<int64_t>(n)),
           line, col});
      for (auto v : compiled) emit_value_release(v);
      return make_string(s);
    }
  }

  if (ns == "Random") {
    if (method == "seed" && argsAst.nodes.size() == 1) {
      auto n = value_to_long(compile(*argsAst.nodes[0]));
      emit_call(module_->getFunction(rt::random_seed), {n});
      return make_nil();
    }
    if (method == "int" && argsAst.nodes.size() == 2) {
      auto lo = value_to_long(compile(*argsAst.nodes[0]));
      auto hi = value_to_long(compile(*argsAst.nodes[1]));
      auto r = emit_call(
          module_->getFunction(rt::random_int), {lo, hi, line, col});
      return make_long(r);
    }
    auto num_pair_call = [&](const char* rt_name) -> llvm::Value* {
      if (argsAst.nodes.size() != 2) return nullptr;
      auto a = compile(*argsAst.nodes[0]);
      auto b = compile(*argsAst.nodes[1]);
      auto r = emit_call(
          module_->getFunction(rt_name),
          {extract_tag(a), extract_data(a),
           extract_tag(b), extract_data(b)});
      emit_value_release(a);
      emit_value_release(b);
      return r;
    };
    if (method == "uniform") {
      if (auto v = num_pair_call(rt::random_uniform)) return v;
    }
    if (method == "gauss") {
      if (auto v = num_pair_call(rt::random_gauss)) return v;
    }
    if (method == "shuffle" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "Array", "Random.shuffle argument");
      auto ap = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      emit_call(module_->getFunction(rt::random_shuffle), {ap});
      emit_value_release(arg);
      return make_nil();
    }
    if (method == "weighted_choice" && argsAst.nodes.size() == 2) {
      auto pop = compile(*argsAst.nodes[0]);
      emit_type_check(pop, "Array", "Random.weighted_choice population");
      auto wts = compile(*argsAst.nodes[1]);
      emit_type_check(wts, "Array", "Random.weighted_choice weights");
      auto pp = builder_.CreateIntToPtr(extract_data(pop), ptrTy);
      auto wp = builder_.CreateIntToPtr(extract_data(wts), ptrTy);
      auto r = emit_call(
          module_->getFunction(rt::random_weighted_choice),
          {pp, wp, line, col});
      emit_value_release(pop);
      emit_value_release(wts);
      return r;
    }
  }

  if (ns == "JSON") {
    using namespace peg::udl;

    // Dynamic `**variable` splat: enumerate at runtime via the
    // kwarg adapter (`culebra_runtime_json_{stringify,parse}_kw`).
    // Literal `**{...}` flattens at compile time below.
    bool has_dynamic_splat = false;
    for (auto& child : argsAst.nodes) {
      if (child->tag == "KWARG_SPLAT"_ &&
          child->nodes[0]->tag != "OBJECT"_) {
        has_dynamic_splat = true;
        break;
      }
    }
    if (has_dynamic_splat && (method == "stringify" || method == "parse")) {
      return compile_json_kwargs_adapter(jit, method, argsAst);
    }

    // Two-pass scan matching the interp resolver: splats merge first
    // (later splat wins), then explicit kwargs layer on top regardless
    // of source order. Literal `**{...}` only; dynamic splats handled
    // above through the runtime adapter.
    std::vector<const peg::Ast*> positional;
    std::map<std::string_view, const peg::Ast*> kwargs;
    std::vector<std::pair<std::string_view, const peg::Ast*>>
        explicit_kwargs;
    std::set<std::string_view> seen_explicit;
    bool saw_named = false;
    for (auto& child : argsAst.nodes) {
      if (child->tag == "KWARG_SPLAT"_) {
        saw_named = true;
        const auto& operand = *child->nodes[0];
        if (operand.tag != "OBJECT"_) {
          throw culebra::CulebraError("SyntaxError",
              "JIT JSON does not yet support dynamic **splat "
              "(use a literal Object or run without --jit)",
              argsAst.line, argsAst.column);
        }
        for (auto& prop : operand.nodes) {
          const auto& key_node = *prop->nodes[1];
          if (key_node.tag != "IDENTIFIER"_) {
            throw culebra::CulebraError("TypeError",
                "**: splat Object key must be an identifier",
                argsAst.line, argsAst.column);
          }
          const peg::Ast* val_ast = prop->nodes.size() >= 3
              ? prop->nodes[2].get()
              : &key_node;
          kwargs[key_node.token] = val_ast;
        }
        continue;
      }
      if (child->tag == "KWARG"_) {
        saw_named = true;
        auto name = child->nodes[0]->token;
        if (!seen_explicit.insert(name).second) {
          throw culebra::CulebraError("TypeError",
              std::format("duplicate keyword argument '{}'", name),
              argsAst.line, argsAst.column);
        }
        explicit_kwargs.emplace_back(name, child->nodes[1].get());
      } else {
        if (saw_named) {
          throw culebra::CulebraError("SyntaxError",
              "positional argument follows keyword argument",
              argsAst.line, argsAst.column);
        }
        positional.push_back(child.get());
      }
    }
    for (const auto& [name, val] : explicit_kwargs) {
      kwargs[name] = val;
    }
    auto take_kwarg = [&](std::string_view name) -> const peg::Ast* {
      auto it = kwargs.find(name);
      if (it == kwargs.end()) return nullptr;
      auto* ast = it->second;
      kwargs.erase(it);
      return ast;
    };
    auto check_no_unknown = [&]() {
      if (!kwargs.empty()) {
        throw culebra::CulebraError("TypeError",
            std::format("unknown keyword argument '{}'",
                        kwargs.begin()->first),
            argsAst.line, argsAst.column);
      }
    };

    if (method == "stringify" &&
        positional.size() <= 2 && !positional.empty()) {
      auto arg = compile(*positional[0]);
      // indent: positional[1] (legacy) or `indent:` kwarg.
      llvm::Value* indent = builder_.getInt64(0);
      llvm::Value* indent_val = nullptr;
      if (positional.size() == 2) {
        indent_val = compile(*positional[1]);
        emit_type_check(indent_val, "Long",
                        "JSON.stringify indent argument");
        indent = extract_data(indent_val);
      } else if (auto* in_ast = take_kwarg("indent")) {
        indent_val = compile(*in_ast);
        emit_type_check(indent_val, "Long",
                        "JSON.stringify indent argument");
        indent = extract_data(indent_val);
      }
      // Bool kwargs: sort_keys, lines. Both default to false; both can
      // be dynamic expressions (typechecked at runtime in helper).
      auto compile_bool_kwarg = [&](const char* name, const char* where,
                                     llvm::Value*& slot,
                                     llvm::Value*& owned_val) {
        if (auto* ast = take_kwarg(name)) {
          owned_val = compile(*ast);
          emit_type_check(owned_val, "Bool", where);
          slot = builder_.CreateTrunc(extract_data(owned_val),
                                       builder_.getInt8Ty());
        }
      };
      llvm::Value* sort_keys = builder_.getInt8(0);
      llvm::Value* sort_val = nullptr;
      compile_bool_kwarg("sort_keys", "JSON.stringify sort_keys argument",
                         sort_keys, sort_val);
      llvm::Value* lines = builder_.getInt8(0);
      llvm::Value* lines_val = nullptr;
      compile_bool_kwarg("lines", "JSON.stringify lines argument",
                         lines, lines_val);
      check_no_unknown();
      auto s = emit_call(
          module_->getFunction(rt::json_stringify),
          {extract_tag(arg), extract_data(arg), indent, sort_keys, lines});
      emit_value_release(arg);
      if (indent_val) emit_value_release(indent_val);
      if (sort_val) emit_value_release(sort_val);
      if (lines_val) emit_value_release(lines_val);
      return make_string(s);
    }

    if (method == "parse" && positional.size() == 1) {
      auto arg = compile(*positional[0]);
      emit_type_check(arg, "String", "JSON.parse argument");
      auto sp = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      // number_mode: pass as String pointer. Default literal "auto".
      llvm::Value* modePtr =
          builder_.CreateGlobalString("auto", ".json.mode.auto");
      llvm::Value* mode_val = nullptr;
      if (auto* nm_ast = take_kwarg("number_mode")) {
        mode_val = compile(*nm_ast);
        emit_type_check(mode_val, "String",
                        "JSON.parse number_mode argument");
        modePtr = builder_.CreateIntToPtr(extract_data(mode_val), ptrTy);
      }
      // lines: Bool kwarg, dynamic-typechecked.
      llvm::Value* lines = builder_.getInt8(0);
      llvm::Value* lines_val = nullptr;
      if (auto* ln_ast = take_kwarg("lines")) {
        lines_val = compile(*ln_ast);
        emit_type_check(lines_val, "Bool",
                        "JSON.parse lines argument");
        lines = builder_.CreateTrunc(extract_data(lines_val),
                                      builder_.getInt8Ty());
      }
      check_no_unknown();
      auto v = emit_call(
          module_->getFunction(rt::json_parse), {sp, modePtr, lines});
      emit_value_release(arg);
      if (mode_val) emit_value_release(mode_val);
      if (lines_val) emit_value_release(lines_val);
      return v;
    }
  }

  if (ns == "Sys") {
    if (method == "exit" && argsAst.nodes.size() == 1) {
      auto code = value_to_long(compile(*argsAst.nodes[0]));
      emit_call(module_->getFunction(rt::sys_exit), {code});
      return make_nil();  // unreachable; sys_exit terminates the process
    }
    if (method == "env" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "Sys.env argument");
      auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto s = emit_call(module_->getFunction(rt::sys_env), {ptr});
      emit_value_release(arg);
      return make_string(s);
    }
    if (method == "time" && argsAst.nodes.size() == 0) {
      auto t = emit_call(module_->getFunction(rt::sys_time), {});
      return make_float(t);
    }
  }

  if (ns == "Tensor") {
    auto pack_args = [&]() {
      auto n = argsAst.nodes.size();
      llvm::IRBuilder<> entryB(
          &builder_.GetInsertBlock()->getParent()->getEntryBlock(),
          builder_.GetInsertBlock()->getParent()->getEntryBlock().begin());
      auto argsAlloca = entryB.CreateAlloca(
          valueType_, builder_.getInt64(static_cast<int64_t>(std::max<size_t>(n, 1))),
          "tensor.args");
      std::vector<llvm::Value*> compiled;
      compiled.reserve(n);
      for (size_t i = 0; i < n; i++) {
        auto v = compile(*argsAst.nodes[i]);
        auto slot = builder_.CreateGEP(valueType_, argsAlloca,
                                       builder_.getInt64(i));
        builder_.CreateStore(v, slot);
        compiled.push_back(v);
      }
      return std::make_pair(argsAlloca, std::move(compiled));
    };

    if (method == "zeros" || method == "ones" || method == "randn") {
      auto [argsAlloca, compiled] = pack_args();
      const char* rt_name = method == "zeros" ? rt::tensor_zeros
                          : method == "ones"  ? rt::tensor_ones
                                              : rt::tensor_randn;
      auto t = emit_call(
          module_->getFunction(rt_name),
          {argsAlloca,
           builder_.getInt64(static_cast<int64_t>(argsAst.nodes.size())),
           line, col});
      for (auto v : compiled) emit_value_release(v);
      return make_tensor(t);
    }
    if (method == "from" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "Array", "Tensor.from argument");
      auto ap = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto t = emit_call(
          module_->getFunction(rt::tensor_from), {ap, line, col});
      emit_value_release(arg);
      return make_tensor(t);
    }
    if (method == "from_csv" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "Tensor.from_csv argument");
      auto pp = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto t = emit_call(
          module_->getFunction(rt::tensor_from_csv), {pp});
      emit_value_release(arg);
      return make_tensor(t);
    }
    if ((method == "sigmoid" || method == "relu" || method == "softmax") &&
        argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "Tensor",
                      method == "sigmoid" ? "Tensor.sigmoid argument"
                    : method == "relu"    ? "Tensor.relu argument"
                                          : "Tensor.softmax argument");
      auto ap = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      int op_id = method == "sigmoid" ? static_cast<int>(culebra::Op::Sigmoid)
                : method == "relu"    ? static_cast<int>(culebra::Op::Relu)
                                      : static_cast<int>(culebra::Op::Softmax);
      auto t = emit_call(
          module_->getFunction(rt::tensor_unary),
          {ap, builder_.getInt64(op_id)});
      emit_value_release(arg);
      return make_tensor(t);
    }
    if (method == "eval") {
      // Variadic: each arg must be a Tensor; we just call eval_one
      // on each and release. No alloca needed.
      for (size_t i = 0; i < argsAst.nodes.size(); i++) {
        auto v = compile(*argsAst.nodes[i]);
        emit_type_check(v, "Tensor", "Tensor.eval argument");
        auto p = builder_.CreateIntToPtr(extract_data(v), ptrTy);
        emit_call(module_->getFunction(rt::tensor_eval_one), {p});
        emit_value_release(v);
      }
      return make_nil();
    }
  }

  return nullptr;
}

inline llvm::Value* JitExtension::compile_json_kwargs_adapter(
    JIT& jit, std::string_view method, const peg::Ast& argsAst) {
  CULEBRA_JIT_EXT_BODY_ALIASES(jit);
  using namespace peg::udl;
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto i64Ty = builder_.getInt64Ty();

  // Scan ARG_LIST. JSON adapters expect exactly one positional (v/s).
  std::vector<const peg::Ast*> positional;
  std::vector<std::pair<std::string_view, const peg::Ast*>>
      explicit_kwargs;
  std::set<std::string_view> seen_explicit;
  std::vector<const peg::Ast*> splats;
  bool saw_named = false;
  for (auto& child : argsAst.nodes) {
    if (child->tag == "KWARG_SPLAT"_) {
      saw_named = true;
      splats.push_back(child->nodes[0].get());
    } else if (child->tag == "KWARG"_) {
      saw_named = true;
      auto name = child->nodes[0]->token;
      if (!seen_explicit.insert(name).second) {
        throw culebra::CulebraError("TypeError",
            std::format("duplicate keyword argument '{}'", name),
            argsAst.line, argsAst.column);
      }
      explicit_kwargs.emplace_back(name, child->nodes[1].get());
    } else {
      if (saw_named) {
        throw culebra::CulebraError("SyntaxError",
            "positional argument follows keyword argument",
            argsAst.line, argsAst.column);
      }
      positional.push_back(child.get());
    }
  }
  if (positional.size() != 1) {
    throw culebra::CulebraError("ArityError",
        std::format("JSON.{}: expected exactly one positional argument "
                    "(the value)", method),
        argsAst.line, argsAst.column);
  }

  // Compile every value once.
  auto v_val = compile(*positional[0]);
  std::vector<llvm::Constant*> kwKeys;
  std::vector<llvm::Value*> kwVals;
  for (auto& [name, ast] : explicit_kwargs) {
    kwKeys.push_back(builder_.CreateGlobalString(
        std::string(name), ".kwkey"));
    kwVals.push_back(compile(*ast));
  }
  std::vector<llvm::Value*> splatVals;
  for (auto* ast : splats) splatVals.push_back(compile(*ast));

  auto fn = builder_.GetInsertBlock()->getParent();
  llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                            fn->getEntryBlock().begin());
  auto alloc_slab = [&](llvm::Type* ty, size_t n,
                         const char* name) -> llvm::Value* {
    if (n == 0) return llvm::ConstantPointerNull::get(ptrTy);
    return entryB.CreateAlloca(
        ty, builder_.getInt64(static_cast<int64_t>(n)), name);
  };
  auto store_at = [&](llvm::Value* base, llvm::Type* ty, size_t i,
                       llvm::Value* val) {
    auto slot = builder_.CreateInBoundsGEP(
        ty, base, {builder_.getInt64(static_cast<int64_t>(i))});
    builder_.CreateStore(val, slot);
  };
  auto keysSlab = alloc_slab(ptrTy, kwKeys.size(), "json.kw.keys");
  auto valsSlab = alloc_slab(jit.valueType_, kwVals.size(), "json.kw.vals");
  auto splatSlab = alloc_slab(jit.valueType_, splatVals.size(),
                               "json.kw.splat");
  for (size_t i = 0; i < kwKeys.size(); i++) {
    store_at(keysSlab, ptrTy, i, kwKeys[i]);
    store_at(valsSlab, jit.valueType_, i, kwVals[i]);
  }
  for (size_t i = 0; i < splatVals.size(); i++) {
    store_at(splatSlab, jit.valueType_, i, splatVals[i]);
  }

  auto lineV = jit.current_line_val();
  auto colV = jit.current_column_val();
  if (method == "stringify") {
    auto s = emit_call(
        module_->getOrInsertFunction(
            rt::json_stringify_kw, ptrTy, builder_.getInt8Ty(), i64Ty,
            i64Ty, ptrTy, ptrTy, i64Ty, ptrTy, i64Ty, i64Ty),
        {extract_tag(v_val), extract_data(v_val),
         builder_.getInt64(static_cast<int64_t>(kwVals.size())),
         keysSlab, valsSlab,
         builder_.getInt64(static_cast<int64_t>(splatVals.size())),
         splatSlab, lineV, colV});
    return make_string(s);
  }
  // method == "parse"
  emit_type_check(v_val, "String", "JSON.parse argument");
  auto sPtr = builder_.CreateIntToPtr(extract_data(v_val), ptrTy);
  auto v = emit_call(
      module_->getOrInsertFunction(
          rt::json_parse_kw, jit.valueType_, ptrTy,
          i64Ty, ptrTy, ptrTy, i64Ty, ptrTy, i64Ty, i64Ty),
      {sPtr,
       builder_.getInt64(static_cast<int64_t>(kwVals.size())),
       keysSlab, valsSlab,
       builder_.getInt64(static_cast<int64_t>(splatVals.size())),
       splatSlab, lineV, colV});
  // parse_kw doesn't consume the cstring (TAG_STRING is non-refcounted);
  // matches plain json_parse semantics.
  emit_value_release(v_val);
  return v;
}

inline llvm::Value* JitExtension::compile_ns_prop(JIT& jit,
                                                    std::string_view ns,
                                                    std::string_view prop) {
  CULEBRA_JIT_EXT_BODY_ALIASES(jit);
  if (ns == "Sys" && prop == "argv") {
    auto arr = emit_call(module_->getFunction(rt::sys_argv), {});
    return make_array(arr);
  }
  if (ns == "Math") {
    auto emit = [&](double d) {
      return make_float(llvm::ConstantFP::get(builder_.getDoubleTy(), d));
    };
    if (prop == "pi")  return emit(M_PI);
    if (prop == "e")   return emit(M_E);
    if (prop == "inf") return emit(std::numeric_limits<double>::infinity());
    if (prop == "nan") return emit(std::numeric_limits<double>::quiet_NaN());
  }
  return nullptr;
}

inline llvm::Value* JitExtension::emit_output_call(JIT& jit,
                                                     const char* rt_name,
                                                     const peg::Ast& argsAst) {
  auto arg = jit.compile(*argsAst.nodes[0]);
  jit.emit_call(jit.module_->getFunction(rt_name),
                      {jit.extract_tag(arg), jit.extract_data(arg)});
  jit.emit_value_release(arg);
  return jit.make_nil();
}

inline bool JitExtension::is_builtin_var(const std::string& name) {
  static const std::unordered_set<std::string_view> names = {
      "puts",    "print",     "assert",
      "to_long", "to_float",  "to_string", "type_of",
      "Math",    "IO",        "FS",        "Random",  "Sys",  "JSON"};
  return names.contains(name);
}

inline llvm::Value* JitExtension::compile_ufcs_builtin(
    JIT& jit, const std::string& method, const peg::Ast& argsAst,
    llvm::Value* receiver) {
  if (argsAst.nodes.size() != 0) return nullptr;
  CULEBRA_JIT_EXT_BODY_ALIASES(jit);
  auto line = jit.current_line_val();
  auto col = jit.current_column_val();
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  if (method == "puts" || method == "print") {
    auto rt_name = method == "puts" ? rt::puts : rt::print;
    emit_call(module_->getFunction(rt_name),
                        {extract_tag(receiver), extract_data(receiver)});
    emit_value_release(receiver);
    return make_nil();
  }
  if (method == "assert") {
    emit_call(
        module_->getFunction(rt::assert_),
        {extract_tag(receiver), extract_data(receiver), line, col});
    emit_value_release(receiver);
    return make_nil();
  }
  if (method == "to_long") {
    emit_type_check(receiver, "String", "to_long argument");
    auto strPtr =
        builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto r = emit_call(
        module_->getFunction(rt::to_long), {strPtr, line, col});
    emit_value_release(receiver);
    return make_long(r);
  }
  if (method == "to_string") {
    auto s = emit_call(
        module_->getFunction(rt::value_to_display),
        {extract_tag(receiver), extract_data(receiver)});
    emit_value_release(receiver);
    return make_string(s);
  }
  if (method == "type_of") {
    auto s = emit_call(module_->getFunction(rt::type_of),
                                 {extract_tag(receiver)});
    emit_value_release(receiver);
    return make_string(s);
  }
  return nullptr;
}

#undef CULEBRA_JIT_EXT_BODY_ALIASES

}  // namespace culebra
