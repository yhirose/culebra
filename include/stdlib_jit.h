#pragma once

// JIT-side implementation of the Culebra standard library.
//
// Independent header. Include from main.cc (or any embedder) after
// jit.h. Provides the runtime functions called from JIT'd code and the
// `JitExtension` struct that fills `JIT::ExtensionHooks`. Embedders
// install the stdlib by calling `culebra::install_jit_stdlib()` once
// before `JIT::run()`.

#include <jit.h>
#include <proc.h>
#include <shared.h>
#include <regexlib.h>
#include <sendable_jit.h>  // JIT isolate transfer (jit_serialize, spawn, handle)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <system_error>

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
  if (tag == TAG_STRING || tag == TAG_STRINGVIEW) {
    auto sv = _culebra_str_view(tag, data);
    return {TAG_LONG,
            parse_long_strict(std::string(sv).c_str(), line, col)};
  }
  throw_type_error_at(line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_to_float_any(
    int8_t tag, int64_t data, int64_t line, int64_t col) {
  if (tag == TAG_FLOAT) return {TAG_FLOAT, data};
  if (tag == TAG_LONG) {
    return {TAG_FLOAT, _culebra_double_to_bits(static_cast<double>(data))};
  }
  if (tag == TAG_STRING || tag == TAG_STRINGVIEW) {
    auto sv = _culebra_str_view(tag, data);
    auto d = parse_double_strict(std::string(sv).c_str(), line, col);
    return {TAG_FLOAT, _culebra_double_to_bits(d)};
  }
  throw_type_error_at(line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_type_of(int8_t tag) {
  // _culebra_tag_name returns a static C-string literal (no length header).
  // Intern it so the surfaced TAG_STRING value carries a header like any
  // other String.
  return _intern_str(_culebra_tag_name(tag));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_print(int8_t type,
                                                        int64_t data) {
  if (auto s = _try_str_special(type, data)) {
    std::cout << *s;
    return;
  }
  if (type == TAG_STRING) {
    auto* p = reinterpret_cast<const char*>(data);
    std::cout.write(p, static_cast<std::streamsize>(_str_len(p)));
  } else if (type == TAG_STRINGVIEW) {
    auto* v = reinterpret_cast<const JitStringView*>(data);
    std::cout.write(v->ptr, static_cast<std::streamsize>(v->len));
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
        std::format("FS.read: cannot open '{}' at {}:{}.", path, line, col),
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
        std::format("FS.write: cannot open '{}' at {}:{}.", path, line, col),
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

// recursive remove (`FS.remove(path, recursive: true)`).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_fs_remove_all(
    const char* path, int64_t line, int64_t col) {
  std::error_code ec;
  if (std::filesystem::remove_all(path ? path : "", ec) ==
          static_cast<std::uintmax_t>(-1) || ec) {
    _fs_throw_io(std::format("FS.remove('{}', recursive: true)",
                             path ? path : ""), line, col, ec);
  }
}

// `FS.stat(path)` -> Object{size, is_dir, is_file, is_symlink, mtime}.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_fs_stat(
    const char* path, int64_t line, int64_t col) {
  std::error_code ec;
  auto st = std::filesystem::symlink_status(path ? path : "", ec);
  if (ec || st.type() == std::filesystem::file_type::not_found) {
    _fs_throw_io(std::format("FS.stat('{}')", path ? path : ""), line, col, ec);
  }
  bool is_link = st.type() == std::filesystem::file_type::symlink;
  auto fst = std::filesystem::status(path ? path : "", ec);
  std::uintmax_t sz = 0;
  if (std::filesystem::is_regular_file(fst)) {
    sz = std::filesystem::file_size(path ? path : "", ec);
    if (ec) sz = 0;
  }
  int64_t mtime = culebra::_fs_mtime_secs(path ? path : "");
  auto* o = culebra_runtime_object_new();
  culebra_runtime_object_set(o, "size", false, TAG_LONG,
                             static_cast<int64_t>(sz), line, col);
  culebra_runtime_object_set(o, "is_dir", false, TAG_BOOL,
                             std::filesystem::is_directory(fst) ? 1 : 0,
                             line, col);
  culebra_runtime_object_set(o, "is_file", false, TAG_BOOL,
                             std::filesystem::is_regular_file(fst) ? 1 : 0,
                             line, col);
  culebra_runtime_object_set(o, "is_symlink", false, TAG_BOOL,
                             is_link ? 1 : 0, line, col);
  culebra_runtime_object_set(o, "mtime", false, TAG_LONG, mtime, line, col);
  return o;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_fs_rename(
    const char* src, const char* dst, int64_t line, int64_t col) {
  std::error_code ec;
  std::filesystem::rename(src ? src : "", dst ? dst : "", ec);
  if (ec) _fs_throw_io(std::format("FS.rename('{}', '{}')", src ? src : "",
                                   dst ? dst : ""), line, col, ec);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_fs_copy(
    const char* src, const char* dst, int64_t recursive,
    int64_t line, int64_t col) {
  std::error_code ec;
  auto opts = std::filesystem::copy_options::overwrite_existing;
  if (recursive) opts |= std::filesystem::copy_options::recursive;
  std::filesystem::copy(src ? src : "", dst ? dst : "", opts, ec);
  if (ec) _fs_throw_io(std::format("FS.copy('{}', '{}')", src ? src : "",
                                   dst ? dst : ""), line, col, ec);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_fs_normpath(
    const char* path) {
  return _culebra_heap_str(
      std::filesystem::path(path ? path : "").lexically_normal().string());
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_fs_is_abs(
    const char* path) {
  return std::filesystem::path(path ? path : "").is_absolute() ? 1 : 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_fs_abspath(
    const char* path, int64_t line, int64_t col) {
  std::error_code ec;
  auto out = std::filesystem::absolute(path ? path : "", ec);
  if (ec) _fs_throw_io(std::format("FS.abspath('{}')", path ? path : ""),
                       line, col, ec);
  return _culebra_heap_str(out.lexically_normal().string());
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_fs_realpath(
    const char* path, int64_t line, int64_t col) {
  std::error_code ec;
  auto out = std::filesystem::weakly_canonical(path ? path : "", ec);
  if (ec) _fs_throw_io(std::format("FS.realpath('{}')", path ? path : ""),
                       line, col, ec);
  return _culebra_heap_str(out.string());
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_fs_is_symlink(
    const char* path) {
  std::error_code ec;
  return std::filesystem::is_symlink(
      std::filesystem::symlink_status(path ? path : "", ec)) ? 1 : 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_fs_symlink(
    const char* target, const char* link, int64_t line, int64_t col) {
  std::error_code ec;
  std::filesystem::create_symlink(target ? target : "", link ? link : "", ec);
  if (ec) _fs_throw_io(std::format("FS.symlink('{}', '{}')",
                                   target ? target : "", link ? link : ""),
                       line, col, ec);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_fs_readlink(
    const char* path, int64_t line, int64_t col) {
  std::error_code ec;
  auto out = std::filesystem::read_symlink(path ? path : "", ec);
  if (ec) _fs_throw_io(std::format("FS.readlink('{}')", path ? path : ""),
                       line, col, ec);
  return _culebra_heap_str(out.string());
}

// `FS.walk(path)` -> Array<String>, recursive depth-first.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_fs_walk(
    const char* path, int64_t line, int64_t col) {
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(path ? path : "", ec);
  if (ec) _fs_throw_io(std::format("FS.walk('{}')", path ? path : ""),
                       line, col, ec);
  auto* arr = culebra_runtime_array_new();
  std::error_code iter_ec;
  for (auto end = std::filesystem::recursive_directory_iterator();
       it != end; it.increment(iter_ec)) {
    if (iter_ec) break;
    auto* s = _culebra_heap_str(it->path().string());
    culebra_runtime_array_push(arr, TAG_STRING, reinterpret_cast<int64_t>(s));
  }
  return arr;
}

// `FS.glob(pattern)` -> Array<String>, shares culebra::_fs_glob with interp.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_fs_glob(
    const char* pattern) {
  auto* arr = culebra_runtime_array_new();
  for (auto& m : culebra::_fs_glob(pattern ? pattern : "")) {
    auto* s = _culebra_heap_str(m);
    culebra_runtime_array_push(arr, TAG_STRING, reinterpret_cast<int64_t>(s));
  }
  return arr;
}

// --- _Time primitives ---
// Calendar logic is shared with the interpreter via `culebra::_time_detail`
// (see stdlib_interp.h). The user-facing `Time` module (Instant /
// Duration classes) is built from culebra source (`TIME_MODULE_SOURCE`
// in stdlib_interp.h) — interp registers it lazily, JIT/AOT prepend it
// selectively via `prepend_stdlib_preamble_selective`. Timestamps are
// i64 nanos since Unix epoch; `monotonic` / `sleep` stay Float
// (measurement, precision-insensitive).

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_time_now_nanos() {
  using clock = std::chrono::system_clock;
  auto d = clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE double culebra_runtime_time_monotonic() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  auto d = clock::now() - t0;
  return std::chrono::duration<double>(d).count();
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_time_sleep(double secs) {
  if (secs > 0) {
    std::this_thread::sleep_for(std::chrono::duration<double>(secs));
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_time_from_iso_nanos(
    const char* s, int64_t line, int64_t col) {
  auto r = culebra::_time_detail::parse_iso_nanos(s ? s : "");
  if (!r) {
    throw culebra::CulebraError("ValueError",
        std::format("_Time.from_iso_nanos: invalid ISO 8601 '{}' at {}:{}.",
                    s ? s : "", line, col),
        line, col);
  }
  return *r;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_time_parse_nanos(
    const char* s, const char* fmt, int64_t line, int64_t col) {
  std::tm tm{};
  if (!strptime(s ? s : "", fmt ? fmt : "", &tm)) {
    throw culebra::CulebraError("ValueError",
        std::format("_Time.parse_nanos: '{}' does not match '{}' at {}:{}.",
                    s ? s : "", fmt ? fmt : "", line, col),
        line, col);
  }
  tm.tm_isdst = -1;
  auto t = std::mktime(&tm);
  return culebra::_time_detail::combine_nanos(t, 0);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_time_iso_nanos(
    int64_t nanos, int64_t utc) {
  return _culebra_heap_str(culebra::_time_detail::format_iso_nanos(nanos, utc != 0));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_time_format_nanos(
    int64_t nanos, const char* fmt, int64_t utc) {
  return _culebra_heap_str(culebra::_time_detail::format_strftime_nanos(
      nanos, fmt ? fmt : "", utc != 0));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_time_weekday_nanos(
    int64_t nanos, int64_t utc) {
  return culebra::_time_detail::iso_weekday(
      culebra::_time_detail::to_tm_nanos(nanos, utc != 0));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject*
culebra_runtime_time_parts_nanos(int64_t nanos, int64_t utc) {
  auto tm = culebra::_time_detail::to_tm_nanos(nanos, utc != 0);
  auto sub = culebra::_time_detail::split_nanos(nanos).second;
  auto* o = culebra_runtime_object_new();
  auto put = [&](const char* k, int64_t v) {
    culebra_runtime_object_set(o, k, /*mut=*/false, TAG_LONG, v, 0, 0);
  };
  put("year",       tm.tm_year + 1900);
  put("month",      tm.tm_mon + 1);
  put("day",        tm.tm_mday);
  put("hour",       tm.tm_hour);
  put("minute",     tm.tm_min);
  put("second",     tm.tm_sec);
  put("nanosecond", sub);
  put("weekday",    culebra::_time_detail::iso_weekday(tm));
  put("dayofyear",  tm.tm_yday + 1);
  return o;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_time_from_parts_nanos(
    JitObject* o, int64_t utc) {
  auto get_long = [&](const char* k, int64_t fallback) -> int64_t {
    auto* entry = _find_property(o, k);
    if (!entry) return fallback;
    return entry->value.tag == TAG_LONG ? entry->value.data : fallback;
  };
  std::tm tm{};
  tm.tm_year = static_cast<int>(get_long("year", 1970) - 1900);
  tm.tm_mon  = static_cast<int>(get_long("month", 1) - 1);
  tm.tm_mday = static_cast<int>(get_long("day", 1));
  tm.tm_hour = static_cast<int>(get_long("hour", 0));
  tm.tm_min  = static_cast<int>(get_long("minute", 0));
  tm.tm_sec  = static_cast<int>(get_long("second", 0));
  auto sub_ns = get_long("nanosecond", 0);
  return culebra::_time_detail::from_tm_nanos(tm, sub_ns, utc != 0);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_time_add_nanos(
    int64_t nanos, int64_t years, int64_t months,
    int64_t days, int64_t hours, int64_t minutes, int64_t seconds,
    int64_t utc) {
  auto tm = culebra::_time_detail::to_tm_nanos(nanos, utc != 0);
  auto sub = culebra::_time_detail::split_nanos(nanos).second;
  if (years || months) {
    int target_year = tm.tm_year + 1900 + static_cast<int>(years);
    int target_month_total = tm.tm_mon + static_cast<int>(months);
    target_year += target_month_total / 12;
    int target_month = target_month_total % 12;
    if (target_month < 0) { target_month += 12; target_year -= 1; }
    int last = culebra::_time_detail::days_in_month(target_year, target_month + 1);
    int target_day = tm.tm_mday > last ? last : tm.tm_mday;
    tm.tm_year = target_year - 1900;
    tm.tm_mon = target_month;
    tm.tm_mday = target_day;
  }
  tm.tm_mday += static_cast<int>(days);
  tm.tm_hour += static_cast<int>(hours);
  tm.tm_min  += static_cast<int>(minutes);
  tm.tm_sec  += static_cast<int>(seconds);
  return culebra::_time_detail::from_tm_nanos(tm, sub, utc != 0);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_time_start_of_nanos(
    int64_t nanos, const char* unit, int64_t utc, int64_t line, int64_t col) {
  auto tm = culebra::_time_detail::to_tm_nanos(nanos, utc != 0);
  std::string_view u(unit ? unit : "");
  if      (u == "year")   { tm.tm_mon = 0; tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; }
  else if (u == "month")  { tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; }
  else if (u == "day")    { tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; }
  else if (u == "hour")   { tm.tm_min = 0; tm.tm_sec = 0; }
  else if (u == "minute") { tm.tm_sec = 0; }
  else {
    throw culebra::CulebraError(
        "ValueError",
        std::format("_Time.start_of_nanos: unknown unit '{}' "
                    "(year/month/day/hour/minute) at {}:{}.",
                    std::string(u), line, col),
        line, col);
  }
  return culebra::_time_detail::from_tm_nanos(tm, 0, utc != 0);
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
// Append an `env` Object's String-keyed entries as name/value pairs.
// `obj` is borrowed (no ref drop). Returns false on the first non-String
// value (partial fill left in `out`) so the caller can release its owned
// refs before throwing; true if every value was a String.
inline bool _ns_env_object_pairs(
    JitObject* obj, std::vector<std::pair<std::string, std::string>>& out) {
  if (!obj->shape) return true;
  for (size_t k = 0; k < obj->shape->names.size(); k++) {
    const JitValue& sv = obj->slots[k].value;
    if (sv.tag != TAG_STRING && sv.tag != TAG_STRINGVIEW) return false;
    out.emplace_back(std::string(obj->shape->names[k]),
                     std::string(_culebra_str_view(sv.tag, sv.data)));
  }
  return true;
}

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

  // Like take_typed but accepts String or StringView and returns the bytes
  // as an owned std::string (StringView is non-refcounted, so the release is
  // a no-op). Absent → nullopt. Lets built-ins accept either string flavor.
  std::optional<std::string> take_string(std::string_view name) {
    auto it = merged_.find(name);
    if (it == merged_.end()) return std::nullopt;
    auto v = it->second;
    merged_.erase(it);
    if (v.tag != TAG_STRING && v.tag != TAG_STRINGVIEW) {
      _culebra_value_release_impl(v.tag, v.data);
      fail(std::format("{}: '{}' must be String", ctx_, name));
    }
    std::string s(_culebra_str_view(v.tag, v.data));
    _culebra_value_release_impl(v.tag, v.data);
    return s;
  }

  // Resolve an `env` Object-of-String kwarg into name/value pairs (appended to
  // `out`). Returns true if `env` was present. A non-String value is a
  // TypeError tagged with ctx_.
  bool take_env(std::vector<std::pair<std::string, std::string>>& out) {
    auto v = take_typed("env", TAG_OBJECT, "Object");
    if (!v) return false;
    bool ok = _ns_env_object_pairs(reinterpret_cast<JitObject*>(v->data), out);
    _culebra_value_release_impl(v->tag, v->data);
    if (!ok) fail(std::string(ctx_) + ": env values must be String", "TypeError");
    return true;
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

// Build the `{code, stdout, stderr, ok, signal, error}` result Object shared
// by Proc.run/all/race. Spawned => process result with `error` nil; a spawn
// failure => `ok:false` with the message in `error` (Proc.all's allSettled
// error representation).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* _culebra_proc_outcome_to_object(
    const culebra::proc::RunOutcome& oc, int64_t line, int64_t col) {
  auto* o = culebra_runtime_object_new();
  if (oc.spawned) {
    const auto& r = oc.result;
    culebra_runtime_object_set(o, "code", false, TAG_LONG, r.code, line, col);
    culebra_runtime_object_set(o, "stdout", false, TAG_STRING,
        reinterpret_cast<int64_t>(_culebra_heap_str(r.out)), line, col);
    culebra_runtime_object_set(o, "stderr", false, TAG_STRING,
        reinterpret_cast<int64_t>(_culebra_heap_str(r.err)), line, col);
    culebra_runtime_object_set(o, "ok", false, TAG_BOOL, r.ok ? 1 : 0, line, col);
    if (r.signal.empty()) {
      culebra_runtime_object_set(o, "signal", false, TAG_NIL, 0, line, col);
    } else {
      culebra_runtime_object_set(o, "signal", false, TAG_STRING,
          reinterpret_cast<int64_t>(_culebra_heap_str(r.signal)), line, col);
    }
    culebra_runtime_object_set(o, "error", false, TAG_NIL, 0, line, col);
    culebra_runtime_object_set(o, "timed_out", false, TAG_BOOL,
        r.timed_out ? 1 : 0, line, col);
  } else {
    culebra_runtime_object_set(o, "code", false, TAG_LONG, -1, line, col);
    culebra_runtime_object_set(o, "stdout", false, TAG_STRING,
        reinterpret_cast<int64_t>(_culebra_heap_str(std::string(""))), line, col);
    culebra_runtime_object_set(o, "stderr", false, TAG_STRING,
        reinterpret_cast<int64_t>(_culebra_heap_str(std::string(""))), line, col);
    culebra_runtime_object_set(o, "ok", false, TAG_BOOL, 0, line, col);
    culebra_runtime_object_set(o, "signal", false, TAG_NIL, 0, line, col);
    std::string msg = std::format("{} failed: {}", oc.err_what,
                                  std::system_category().message(oc.err_no));
    culebra_runtime_object_set(o, "error", false, TAG_STRING,
        reinterpret_cast<int64_t>(_culebra_heap_str(msg)), line, col);
    culebra_runtime_object_set(o, "timed_out", false, TAG_BOOL, 0, line, col);
  }
  return o;
}

// Parse a TAG_ARRAY of TAG_ARRAY<String> into argv vectors (for Proc.all/race).
// Elements accept String or StringView. Empty inner command => ValueError.
inline std::vector<std::vector<std::string>> _culebra_proc_parse_commands(
    int8_t commands_tag, int64_t commands_data, const char* ctx,
    int64_t line, int64_t col) {
  if (commands_tag != TAG_ARRAY) {
    throw culebra::CulebraError("TypeError",
        std::format("{}: commands must be Array at {}:{}.", ctx, line, col),
        line, col);
  }
  auto* outer = reinterpret_cast<JitArray*>(commands_data);
  std::vector<std::vector<std::string>> commands;
  commands.reserve(outer->size);
  for (size_t i = 0; i < outer->size; i++) {
    const JitValue& cv = outer->items[i];
    if (cv.tag != TAG_ARRAY) {
      throw culebra::CulebraError("TypeError",
          std::format("{}: each command must be an Array of String at {}:{}.",
                      ctx, line, col), line, col);
    }
    auto* inner = reinterpret_cast<JitArray*>(cv.data);
    if (inner->size == 0) {
      throw culebra::CulebraError("ValueError",
          std::format("{}: empty command at {}:{}.", ctx, line, col),
          line, col);
    }
    std::vector<std::string> argv;
    argv.reserve(inner->size);
    for (size_t j = 0; j < inner->size; j++) {
      const JitValue& e = inner->items[j];
      if (e.tag != TAG_STRING && e.tag != TAG_STRINGVIEW) {
        throw culebra::CulebraError("TypeError",
            std::format("{}: command elements must be String at {}:{}.",
                        ctx, line, col), line, col);
      }
      argv.emplace_back(_culebra_str_view(e.tag, e.data));
    }
    commands.push_back(std::move(argv));
  }
  return commands;
}

// Shared core for both Proc.run JIT paths (positional trampoline + kwarg
// adapter). `cmd` must be a TAG_ARRAY of TAG_STRING; cwd/env_over may be
// null. A spawn failure (or `check` on a non-ok result) throws ProcessError;
// a normal non-zero exit / signal death is a `{ok:false}` result Object.
// Does not consume `cmd` — the compile side / trampoline releases it.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue _culebra_proc_run_impl(
    int8_t cmd_tag, int64_t cmd_data, const std::string* cwd,
    const std::vector<std::pair<std::string, std::string>>* env_over,
    const std::string& stdin_data, bool check, int64_t timeout,
    int64_t line, int64_t col) {
  if (cmd_tag != TAG_ARRAY) {
    throw culebra::CulebraError("TypeError",
        std::format("Proc.run: cmd must be Array at {}:{}.", line, col),
        line, col);
  }
  auto* cmd = reinterpret_cast<JitArray*>(cmd_data);
  std::vector<std::string> argv;
  argv.reserve(cmd->size);
  for (size_t i = 0; i < cmd->size; i++) {
    const JitValue& e = cmd->items[i];
    if (e.tag != TAG_STRING && e.tag != TAG_STRINGVIEW) {
      throw culebra::CulebraError("TypeError",
          std::format("Proc.run: command elements must be String at {}:{}.",
                      line, col), line, col);
    }
    argv.emplace_back(_culebra_str_view(e.tag, e.data));
  }
  if (argv.empty()) {
    throw culebra::CulebraError("ValueError",
        std::format("Proc.run: empty command at {}:{}.", line, col),
        line, col);
  }

  auto oc = culebra::proc::run_command(argv, cwd, env_over, stdin_data,
                                       timeout > 0 ? timeout : 0);
  if (!oc.spawned) {
    throw culebra::CulebraError("ProcessError",
        std::format("Proc.run: {} failed at {}:{}: {}.", oc.err_what, line, col,
                    std::system_category().message(oc.err_no)),
        line, col);
  }
  if (check && !oc.result.ok) {
    throw culebra::CulebraError("ProcessError",
        std::format("Proc.run: command {} at {}:{}.",
                    culebra::proc::failure_detail(oc.result), line, col),
        line, col);
  }

  return {TAG_OBJECT,
          reinterpret_cast<int64_t>(_culebra_proc_outcome_to_object(oc, line, col))};
}

// Positional Proc.run(cmd) (no kwargs) — used by the trampoline and AOT.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_proc_run(
    int8_t cmd_tag, int64_t cmd_data, int64_t line, int64_t col) {
  return _culebra_proc_run_impl(cmd_tag, cmd_data, nullptr, nullptr,
                                std::string(), false, 0, line, col);
}

// Kwarg adapter for Proc.run. Resolves cwd/env/stdin/check (plus `**splat`)
// via `_JitKwargResolver`, then runs the command. `cmd` is not consumed.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_proc_run_kw(
    int8_t cmd_tag, int64_t cmd_data,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs,
    int64_t line, int64_t col) {
  _JitKwargResolver kw(n_kw, kw_keys, kw_vals, n_splat, splat_objs, line, col,
                       "Proc.run");
  std::string cwd_str;
  const std::string* cwd_ptr = nullptr;
  if (auto v = kw.take_string("cwd")) {
    cwd_str = std::move(*v);
    cwd_ptr = &cwd_str;
  }
  std::string stdin_data;
  if (auto v = kw.take_string("stdin")) {
    stdin_data = std::move(*v);
  }
  bool check = false;
  if (auto v = kw.take_typed("check", TAG_BOOL, "Bool")) {
    check = v->data != 0;
  }
  int64_t timeout = 0;
  if (auto v = kw.take_typed("timeout", TAG_LONG, "Long")) timeout = v->data;
  std::vector<std::pair<std::string, std::string>> overrides;
  const std::vector<std::pair<std::string, std::string>>* env_ptr =
      kw.take_env(overrides) ? &overrides : nullptr;
  kw.validate_consumed();
  return _culebra_proc_run_impl(cmd_tag, cmd_data, cwd_ptr, env_ptr,
                                stdin_data, check, timeout, line, col);
}

// Proc.all core (shared by trampoline + kwarg adapter). `commands` is not
// consumed. Returns an Array of result Objects (allSettled, input order).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue _culebra_proc_all_impl(
    int8_t commands_tag, int64_t commands_data, int64_t limit, int64_t timeout,
    bool fail_fast, int64_t retries, int64_t line, int64_t col) {
  auto commands = _culebra_proc_parse_commands(commands_tag, commands_data,
                                               "Proc.all", line, col);
  if (limit < 0) limit = 0;
  size_t failed = SIZE_MAX;
  auto outcomes = culebra::proc::run_all(
      commands, static_cast<size_t>(limit), nullptr, nullptr, nullptr,
      timeout > 0 ? timeout : 0, fail_fast, &failed, retries > 0 ? retries : 0);
  if (fail_fast && failed != SIZE_MAX) {
    throw culebra::CulebraError("ProcessError",
        std::format("Proc.all: command {} {} at {}:{}.", failed,
                    culebra::proc::outcome_detail(outcomes[failed]), line, col),
        line, col);
  }
  auto* arr = culebra_runtime_array_new();
  for (auto& oc : outcomes) {
    auto* o = _culebra_proc_outcome_to_object(oc, line, col);
    culebra_runtime_array_push(arr, TAG_OBJECT, reinterpret_cast<int64_t>(o));
  }
  return {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
}

// Positional Proc.all(commands) (no kwargs) — trampoline / AOT.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_proc_all(
    int8_t commands_tag, int64_t commands_data, int64_t line, int64_t col) {
  return _culebra_proc_all_impl(commands_tag, commands_data, 0, 0, false, 0,
                                line, col);
}

// Kwarg adapter for Proc.all (resolves `limit`). `commands` is not consumed.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_proc_all_kw(
    int8_t commands_tag, int64_t commands_data,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs, int64_t line, int64_t col) {
  _JitKwargResolver kw(n_kw, kw_keys, kw_vals, n_splat, splat_objs, line, col,
                       "Proc.all");
  int64_t limit = 0;
  if (auto v = kw.take_typed("limit", TAG_LONG, "Long")) limit = v->data;
  int64_t timeout = 0;
  if (auto v = kw.take_typed("timeout", TAG_LONG, "Long")) timeout = v->data;
  bool fail_fast = false;
  if (auto v = kw.take_typed("fail_fast", TAG_BOOL, "Bool"))
    fail_fast = v->data != 0;
  int64_t retries = 0;
  if (auto v = kw.take_typed("retries", TAG_LONG, "Long")) retries = v->data;
  kw.validate_consumed();
  return _culebra_proc_all_impl(commands_tag, commands_data, limit, timeout,
                                fail_fast, retries, line, col);
}

// Proc.race(commands) — first to finish wins, the rest are killed. `commands`
// is not consumed. Empty list throws ValueError.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_proc_race(
    int8_t commands_tag, int64_t commands_data, int64_t line, int64_t col) {
  auto commands = _culebra_proc_parse_commands(commands_tag, commands_data,
                                               "Proc.race", line, col);
  if (commands.empty()) {
    throw culebra::CulebraError("ValueError",
        std::format("Proc.race: empty command list at {}:{}.", line, col),
        line, col);
  }
  auto [winner, oc] = culebra::proc::run_race(commands);
  (void)winner;
  return {TAG_OBJECT,
          reinterpret_cast<int64_t>(_culebra_proc_outcome_to_object(oc, line, col))};
}

// --- Proc.spawn live handle (JIT) ---
// The handle is a JitObject with data slots (_pid/_out/_err/_done/_result) and
// method-closure slots; each method reads `this` (the handle) via the JitFn
// `this` argument. Result is cached on first wait/poll (idempotent); `drop`
// (GC) best-effort reaps a child that was never waited on.

CULEBRA_RT_INLINE JitObject* _culebra_proc_result_to_object(
    culebra::proc::ProcResult&& pr, int64_t line, int64_t col) {
  culebra::proc::RunOutcome oc;
  oc.spawned = true;
  oc.result = std::move(pr);
  return _culebra_proc_outcome_to_object(oc, line, col);
}

CULEBRA_RT_INLINE int64_t _jit_handle_long(JitObject* h, const char* key) {
  size_t i = h->find_slot(key);
  return i == static_cast<size_t>(-1) ? -1 : h->slots[i].value.data;
}
CULEBRA_RT_INLINE bool _jit_handle_done(JitObject* h) {
  size_t i = h->find_slot("_done");
  return i != static_cast<size_t>(-1) &&
         h->slots[i].value.tag == TAG_BOOL && h->slots[i].value.data;
}
// Returns the cached _result with a +1 for the caller.
CULEBRA_RT_INLINE JitValue _jit_handle_cached(JitObject* h) {
  size_t i = h->find_slot("_result");
  JitValue r = (i == static_cast<size_t>(-1)) ? JitValue{TAG_NIL, 0}
                                              : h->slots[i].value;
  culebra_runtime_value_retain(r.tag, r.data);
  return r;
}
// Cache `res_obj` + mark done; returns the result with a +1 for the caller.
// `res_obj` arrives with refcount 1 (object_new); the slot adopts that ref
// (set_or_append doesn't retain), so we only retain once for the caller.
CULEBRA_RT_INLINE JitValue _jit_handle_finish(JitObject* h, JitObject* res_obj) {
  JitValue res{TAG_OBJECT, reinterpret_cast<int64_t>(res_obj)};
  h->set_or_append("_done", JitValue{TAG_BOOL, 1}, true);
  h->set_or_append("_result", res, true);     // slot adopts object_new's +1
  culebra_runtime_value_retain(res.tag, res.data);  // caller's ref
  return res;
}

// wait/poll/kill are invoked via the method ABI (_culebra_invoke_method1),
// which retains `self`; the callee must consume that +1. Each releases `self`
// before returning. (drop, below, is called from the destructor's drop
// protocol, which manages the object's refcount itself — it must NOT release.)
CULEBRA_RT_INLINE JitValue _jit_handle_wait(JitClosure*, JitValue self,
                                            int64_t, JitValue*) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  JitValue ret;
  if (_jit_handle_done(h)) {
    ret = _jit_handle_cached(h);
  } else {
    int out_fd = static_cast<int>(_jit_handle_long(h, "_out"));
    int err_fd = static_cast<int>(_jit_handle_long(h, "_err"));
    auto pr = culebra::proc::wait_handle(_jit_handle_long(h, "_pid"), out_fd,
                                         err_fd);
    ret = _jit_handle_finish(
        h, _culebra_proc_result_to_object(std::move(pr), 0, 0));
  }
  culebra_runtime_value_release(self.tag, self.data);
  return ret;
}
CULEBRA_RT_INLINE JitValue _jit_handle_poll(JitClosure*, JitValue self,
                                            int64_t, JitValue*) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  JitValue ret;
  if (_jit_handle_done(h)) {
    ret = _jit_handle_cached(h);
  } else {
    int status = 0;
    if (!culebra::proc::try_reap(_jit_handle_long(h, "_pid"), status)) {
      ret = {TAG_NIL, 0};  // still running
    } else {
      int out_fd = static_cast<int>(_jit_handle_long(h, "_out"));
      int err_fd = static_cast<int>(_jit_handle_long(h, "_err"));
      auto pr = culebra::proc::drain_reaped(status, out_fd, err_fd);
      ret = _jit_handle_finish(
          h, _culebra_proc_result_to_object(std::move(pr), 0, 0));
    }
  }
  culebra_runtime_value_release(self.tag, self.data);
  return ret;
}
CULEBRA_RT_INLINE JitValue _jit_handle_kill(JitClosure*, JitValue self,
                                            int64_t n, JitValue* args) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int sig = (n >= 1 && args[0].tag == TAG_LONG)
                ? static_cast<int>(args[0].data) : 15;
  if (!_jit_handle_done(h)) {
    culebra::proc::kill_pid(_jit_handle_long(h, "_pid"), sig);
  }
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_NIL, 0};
}
CULEBRA_RT_INLINE JitValue _jit_handle_drop(JitClosure*, JitValue self,
                                            int64_t, JitValue*) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  if (_jit_handle_done(h)) return {TAG_NIL, 0};
  int out_fd = static_cast<int>(_jit_handle_long(h, "_out"));
  int err_fd = static_cast<int>(_jit_handle_long(h, "_err"));
  long pid = _jit_handle_long(h, "_pid");
  culebra::proc::kill_pid(pid, SIGKILL);
  culebra::proc::wait_handle(pid, out_fd, err_fd);
  h->set_or_append("_done", JitValue{TAG_BOOL, 1}, true);
  return {TAG_NIL, 0};
}

CULEBRA_RT_INLINE JitClosure* _jit_make_handle_method(
    JitValue (*fn)(JitClosure*, JitValue, int64_t, JitValue*), size_t arity) {
  auto* cls = new JitClosure();
  cls->refcount = 1;
  cls->fn_ptr = reinterpret_cast<void*>(fn);
  cls->n_captures = 0;
  cls->captures = nullptr;
  cls->arity = arity;
  _gc_register(cls, GC_TAG_FUNC);
  return cls;
}

CULEBRA_RT_INLINE JitValue _culebra_proc_build_handle(long pid, int out_fd,
                                                      int err_fd) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_pid", JitValue{TAG_LONG, pid}, true);
  h->set_or_append("_out", JitValue{TAG_LONG, out_fd}, true);
  h->set_or_append("_err", JitValue{TAG_LONG, err_fd}, true);
  h->set_or_append("_done", JitValue{TAG_BOOL, 0}, true);
  h->set_or_append("_result", JitValue{TAG_NIL, 0}, true);
  auto fn = [&](const char* name, auto* f, size_t ar) {
    h->set_or_append(name,
        JitValue{TAG_FUNC, reinterpret_cast<int64_t>(
            _jit_make_handle_method(f, ar))}, false);
  };
  fn("wait", _jit_handle_wait, 0);
  fn("poll", _jit_handle_poll, 0);
  fn("kill", _jit_handle_kill, 0);
  fn("drop", _jit_handle_drop, 0);
  h->has_drop = true;
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// --- File handle (JIT) ---
// Methods read `_id` (Long) from the handle and operate on the shared
// side table (culebra::_file_* helpers, defined in stdlib_interp.h). Same
// method ABI as the Proc handle: self arrives +1 and is released here,
// except drop (called from the destructor's drop protocol, which manages
// refcount itself — see _jit_handle_drop).
CULEBRA_RT_INLINE JitValue _jit_file_read(JitClosure*, JitValue self,
                                          int64_t n, JitValue* args) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int64_t id = _jit_handle_long(h, "_id");
  std::string out = (n >= 1 && args[0].tag == TAG_LONG)
      ? culebra::_file_read_n(id, args[0].data, 0, 0)
      : culebra::_file_read_all(id, 0, 0);
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(out))};
}
CULEBRA_RT_INLINE JitValue _jit_file_write(JitClosure*, JitValue self,
                                           int64_t n, JitValue* args) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int64_t id = _jit_handle_long(h, "_id");
  if (n >= 1 && (args[0].tag == TAG_STRING || args[0].tag == TAG_STRINGVIEW)) {
    auto sv = _culebra_str_view(args[0].tag, args[0].data);
    culebra::_file_write(id, sv, 0, 0);
  }
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_NIL, 0};
}
CULEBRA_RT_INLINE JitValue _jit_file_flush(JitClosure*, JitValue self,
                                           int64_t, JitValue*) {
  culebra::_file_flush(_jit_handle_long(reinterpret_cast<JitObject*>(self.data),
                                        "_id"), 0, 0);
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_NIL, 0};
}
CULEBRA_RT_INLINE JitValue _jit_file_seek(JitClosure*, JitValue self,
                                          int64_t n, JitValue* args) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int64_t id = _jit_handle_long(h, "_id");
  long off = (n >= 1 && args[0].tag == TAG_LONG) ? args[0].data : 0;
  std::string_view whence = "set";
  if (n >= 2 && (args[1].tag == TAG_STRING || args[1].tag == TAG_STRINGVIEW)) {
    whence = _culebra_str_view(args[1].tag, args[1].data);
  }
  culebra::_file_seek(id, off, whence, 0, 0);
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_NIL, 0};
}
CULEBRA_RT_INLINE JitValue _jit_file_tell(JitClosure*, JitValue self,
                                          int64_t, JitValue*) {
  long pos = culebra::_file_tell(
      _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id"), 0, 0);
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_LONG, pos};
}
CULEBRA_RT_INLINE JitValue _jit_file_close(JitClosure*, JitValue self,
                                           int64_t, JitValue*) {
  culebra::_file_close(_jit_handle_long(reinterpret_cast<JitObject*>(self.data),
                                        "_id"));
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_NIL, 0};
}
CULEBRA_RT_INLINE JitValue _jit_file_drop(JitClosure*, JitValue self,
                                          int64_t, JitValue*) {
  // drop runs from the destructor's drop protocol — must NOT release self.
  culebra::_file_close(_jit_handle_long(reinterpret_cast<JitObject*>(self.data),
                                        "_id"));
  return {TAG_NIL, 0};
}

// lines()/chunks() iterator FastFns. captures: [handle_cell, id_cell]
// (+ n_cell for chunks). handle_cell keeps the File handle alive so an
// anonymous `File.open(p).lines()` isn't GC'd before the loop runs.
inline void _file_lines_fast_fn(JitClosure* cls, JitValue, bool* done,
                                int8_t* out_tag, int64_t* out_data) {
  int64_t id = cls->captures[1]->value.data;
  std::string out;
  if (!culebra::_file_getline(id, out, 0, 0)) { *done = true; return; }
  *done = false;
  *out_tag = TAG_STRING;
  *out_data = reinterpret_cast<int64_t>(_culebra_heap_str(out));
}
inline void _file_chunks_fast_fn(JitClosure* cls, JitValue, bool* done,
                                 int8_t* out_tag, int64_t* out_data) {
  int64_t id = cls->captures[1]->value.data;
  int64_t n = cls->captures[2]->value.data;
  std::string chunk = culebra::_file_read_n(id, n, 0, 0);
  if (chunk.empty()) { *done = true; return; }
  *done = false;
  *out_tag = TAG_STRING;
  *out_data = reinterpret_cast<int64_t>(_culebra_heap_str(chunk));
}

// Build an iterator over a File handle, adding a `dispose` method that
// closes the handle so a broken for-in still releases the fd.
inline JitObject* _file_iter_build(JitValue self, bool chunks, int64_t n) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int64_t id = _jit_handle_long(h, "_id");
  culebra_runtime_value_retain(self.tag, self.data);  // iter keeps handle alive
  auto* handle_cell = culebra_runtime_cell_new(self.tag, self.data);
  auto* id_cell = culebra_runtime_cell_new(TAG_LONG, id);
  JitObject* it;
  if (chunks) {
    auto* n_cell = culebra_runtime_cell_new(TAG_LONG, n);
    it = _iter_wrap_fast<&_file_chunks_fast_fn>({handle_cell, id_cell, n_cell});
  } else {
    it = _iter_wrap_fast<&_file_lines_fast_fn>({handle_cell, id_cell});
  }
  it->set_or_append("dispose",
      JitValue{TAG_FUNC, reinterpret_cast<int64_t>(
          _jit_make_handle_method(_jit_file_close, 0))}, false);
  return it;
}
CULEBRA_RT_INLINE JitValue _jit_file_lines(JitClosure*, JitValue self,
                                           int64_t, JitValue*) {
  // dispose's close needs the handle; transfer self's +1 into the iterator
  // build (it retains again), then the iterator owns the keep-alive ref.
  auto* it = _file_iter_build(self, /*chunks=*/false, 0);
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(it)};
}
CULEBRA_RT_INLINE JitValue _jit_file_chunks(JitClosure*, JitValue self,
                                            int64_t n, JitValue* args) {
  int64_t sz = (n >= 1 && args[0].tag == TAG_LONG) ? args[0].data : 0;
  auto* it = _file_iter_build(self, /*chunks=*/true, sz);
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(it)};
}

CULEBRA_RT_INLINE JitValue _culebra_file_build_handle(int64_t id) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_id", JitValue{TAG_LONG, id}, false);
  auto fn = [&](const char* name, auto* f, size_t ar) {
    h->set_or_append(name,
        JitValue{TAG_FUNC, reinterpret_cast<int64_t>(
            _jit_make_handle_method(f, ar))}, false);
  };
  fn("read", _jit_file_read, 0);
  fn("write", _jit_file_write, 1);
  fn("flush", _jit_file_flush, 0);
  fn("seek", _jit_file_seek, 1);
  fn("tell", _jit_file_tell, 0);
  fn("lines", _jit_file_lines, 0);
  fn("chunks", _jit_file_chunks, 1);
  fn("close", _jit_file_close, 0);
  fn("drop", _jit_file_drop, 0);
  h->has_drop = true;
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// Spawn core: parse argv, spawn detached, return a live handle (or throw).
CULEBRA_RT_INLINE JitValue _culebra_proc_spawn_build(
    int8_t cmd_tag, int64_t cmd_data, const std::string* cwd,
    const std::vector<std::pair<std::string, std::string>>* env_over,
    const std::string& stdin_data, int64_t line, int64_t col) {
  if (cmd_tag != TAG_ARRAY) {
    throw culebra::CulebraError("TypeError",
        std::format("Proc.spawn: cmd must be Array at {}:{}.", line, col),
        line, col);
  }
  auto* cmd = reinterpret_cast<JitArray*>(cmd_data);
  std::vector<std::string> argv;
  argv.reserve(cmd->size);
  for (size_t i = 0; i < cmd->size; i++) {
    const JitValue& e = cmd->items[i];
    if (e.tag != TAG_STRING && e.tag != TAG_STRINGVIEW) {
      throw culebra::CulebraError("TypeError",
          std::format("Proc.spawn: command elements must be String at {}:{}.",
                      line, col), line, col);
    }
    argv.emplace_back(_culebra_str_view(e.tag, e.data));
  }
  if (argv.empty()) {
    throw culebra::CulebraError("ValueError",
        std::format("Proc.spawn: empty command at {}:{}.", line, col),
        line, col);
  }
  auto sr = culebra::proc::spawn_detached(argv, cwd, env_over, stdin_data);
  if (!sr.spawned) {
    throw culebra::CulebraError("ProcessError",
        std::format("Proc.spawn: {} failed at {}:{}: {}.", sr.err_what, line,
                    col, std::system_category().message(sr.err_no)),
        line, col);
  }
  return _culebra_proc_build_handle(sr.pid, sr.out_fd, sr.err_fd);
}

// Positional Proc.spawn(cmd) (no kwargs) — trampoline / AOT.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_proc_spawn(
    int8_t cmd_tag, int64_t cmd_data, int64_t line, int64_t col) {
  return _culebra_proc_spawn_build(cmd_tag, cmd_data, nullptr, nullptr,
                                   std::string(), line, col);
}

// Kwarg adapter for Proc.spawn (cwd/env/stdin). `cmd` is not consumed.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_proc_spawn_kw(
    int8_t cmd_tag, int64_t cmd_data,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs, int64_t line, int64_t col) {
  _JitKwargResolver kw(n_kw, kw_keys, kw_vals, n_splat, splat_objs, line, col,
                       "Proc.spawn");
  std::string cwd_str;
  const std::string* cwd_ptr = nullptr;
  if (auto v = kw.take_string("cwd")) {
    cwd_str = std::move(*v);
    cwd_ptr = &cwd_str;
  }
  std::string stdin_data;
  if (auto v = kw.take_string("stdin")) stdin_data = std::move(*v);
  std::vector<std::pair<std::string, std::string>> overrides;
  const std::vector<std::pair<std::string, std::string>>* env_ptr =
      kw.take_env(overrides) ? &overrides : nullptr;
  kw.validate_consumed();
  return _culebra_proc_spawn_build(cmd_tag, cmd_data, cwd_ptr, env_ptr,
                                   stdin_data, line, col);
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

  // Marshals a `(single positional, kwargs..., **splat)` ARG_LIST into a
  // runtime `_kw` function (cmd_tag, cmd_data, keys/vals slabs, splat slab,
  // line, col). Shared by Proc.run (rt::proc_run_kw) and Proc.all
  // (rt::proc_all_kw); the positional is released after the call.
  static llvm::Value* compile_single_positional_kwargs(
      JIT& jit, const peg::Ast& argsAst, const peg::Ast& callAst,
      const char* ctx, const char* rt_name);

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

// --- Stdlib namespace as first-class JIT object ---
//
// `let m = IO; m.puts(x)` needs IO to be a value, not just a
// compile-time syntactic pattern. Each stdlib namespace gets a
// lazy JitObject whose method slots are closures that trampoline
// into a single dispatcher table. Fast path (`IO.puts(x)` as
// IDENTIFIER.DOT.ARGUMENTS) still goes through compile_ns_call
// in JitExtension and bypasses this entirely.
//
// To add a method: append one row to `kNsMethods` below. The drift
// check in debug builds verifies that every namespace bound by
// `culebra::setup_built_in_functions` is covered here. _Time is
// an internal Time-class ABI, intentionally excluded.
//
// Each adapter is a thin wrapper that unmarshals positional args
// and calls the matching `culebra_runtime_*` helper. line/col are
// passed as 0 — slow path is rare enough that losing call-site
// location in helper errors is acceptable (the fast path keeps
// line/col via `compile_ns_call`).

namespace _ns_adapt {

[[noreturn]] inline void arity_error(const char* ns, const char* method,
                                       int expected, int64_t got) {
  culebra::throw_runtime_error_at(
      "ArityError",
      std::format("{}.{}: expected {} positional argument{}, got {}",
                  ns, method, expected, expected == 1 ? "" : "s", got),
      0, 0);
}

inline const char* take_str(JitValue v) {
  return v.tag == TAG_STRING ? reinterpret_cast<const char*>(v.data) : "";
}
inline JitArray* take_array(JitValue v) {
  return v.tag == TAG_ARRAY ? reinterpret_cast<JitArray*>(v.data) : nullptr;
}
#ifndef CULEBRA_RT_NO_TENSOR
inline JitTensor* take_tensor(JitValue v) {
  return v.tag == TAG_TENSOR ? reinterpret_cast<JitTensor*>(v.data) : nullptr;
}
#endif
inline JitObject* take_object(JitValue v) {
  return v.tag == TAG_OBJECT ? reinterpret_cast<JitObject*>(v.data) : nullptr;
}
inline int64_t take_long(JitValue v) { return v.data; }
inline double take_double(JitValue v) {
  if (v.tag == TAG_FLOAT) return _culebra_float_to_double(v.data);
  return static_cast<double>(v.data);
}
inline int64_t take_bool(JitValue v) { return v.data != 0 ? 1 : 0; }

inline JitValue v_nil()                  { return {TAG_NIL, 0}; }
inline JitValue v_long(int64_t x)        { return {TAG_LONG, x}; }
inline JitValue v_bool(int64_t x)        { return {TAG_BOOL, x != 0 ? 1 : 0}; }
inline JitValue v_float(double x)        {
  return {TAG_FLOAT, _culebra_double_to_bits(x)};
}
inline JitValue v_string(const char* s)  {
  return {TAG_STRING, reinterpret_cast<int64_t>(s)};
}
inline JitValue v_array(JitArray* a)     {
  return {TAG_ARRAY, reinterpret_cast<int64_t>(a)};
}
inline JitValue v_object(JitObject* o)   {
  return {TAG_OBJECT, reinterpret_cast<int64_t>(o)};
}
#ifndef CULEBRA_RT_NO_TENSOR
inline JitValue v_tensor(JitTensor* t)   {
  return {TAG_TENSOR, reinterpret_cast<int64_t>(t)};
}
#endif

}  // namespace _ns_adapt

// --- Per-method adapters ---

// IO
inline JitValue _ns_io_puts(JitValue* a, int64_t) {
  culebra_runtime_puts(a[0].tag, a[0].data);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_io_print(JitValue* a, int64_t) {
  culebra_runtime_print(a[0].tag, a[0].data);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_io_input(JitValue*, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_input());
}
// Whole-file read/write convenience on FS (open+read/write+close). Reuses
// the runtime file helpers; streaming lives on the File handle.
inline JitValue _ns_fs_read(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_read_file(_ns_adapt::take_str(a[0]), 0, 0));
}
inline JitValue _ns_fs_write(JitValue* a, int64_t) {
  culebra_runtime_write_file(_ns_adapt::take_str(a[0]),
                              _ns_adapt::take_str(a[1]), 0, 0);
  return _ns_adapt::v_nil();
}

// File.open(path, mode="r") -> handle. File.with(path, mode, fn) -> block value.
inline JitValue _ns_file_open(JitValue* a, int64_t n) {
  std::string path = _ns_adapt::take_str(a[0]);
  std::string mode = (n >= 2 && (a[1].tag == TAG_STRING ||
                                 a[1].tag == TAG_STRINGVIEW))
      ? std::string(_culebra_str_view(a[1].tag, a[1].data)) : "r";
  int64_t id = culebra::_file_open(path, mode, 0, 0);
  return _culebra_file_build_handle(id);
}
inline JitValue _ns_file_with(JitValue* a, int64_t n) {
  std::string path = _ns_adapt::take_str(a[0]);
  // params: path, mode="r", fn. With 2 positional args, slot 1 is fn.
  std::string mode = "r";
  JitValue fnv;
  if (n >= 3) {
    if (a[1].tag == TAG_STRING || a[1].tag == TAG_STRINGVIEW)
      mode = std::string(_culebra_str_view(a[1].tag, a[1].data));
    fnv = a[2];
  } else {
    fnv = a[1];
  }
  int64_t id = culebra::_file_open(path, mode, 0, 0);
  JitValue handle = _culebra_file_build_handle(id);
  auto* fn = reinterpret_cast<JitClosure*>(fnv.data);
  culebra_runtime_value_retain(handle.tag, handle.data);  // for the call arg
  JitValue result;
  try {
    result = _culebra_invoke1(fn, handle);
  } catch (...) {
    culebra::_file_close(id);
    culebra_runtime_value_release(handle.tag, handle.data);
    throw;
  }
  culebra::_file_close(id);
  culebra_runtime_value_release(handle.tag, handle.data);
  return result;
}

// Math
inline JitValue _ns_math_abs(JitValue* a, int64_t) {
  return culebra_runtime_math_abs(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_log(JitValue* a, int64_t) {
  return culebra_runtime_math_log(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_exp(JitValue* a, int64_t) {
  return culebra_runtime_math_exp(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_sqrt(JitValue* a, int64_t) {
  return culebra_runtime_math_sqrt(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_floor(JitValue* a, int64_t) {
  return culebra_runtime_math_floor(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_ceil(JitValue* a, int64_t) {
  return culebra_runtime_math_ceil(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_round(JitValue* a, int64_t) {
  return culebra_runtime_math_round(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_min(JitValue* a, int64_t n) {
  return culebra_runtime_math_min(a, n, 0, 0);
}
inline JitValue _ns_math_max(JitValue* a, int64_t n) {
  return culebra_runtime_math_max(a, n, 0, 0);
}
inline JitValue _ns_math_pow(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_math_pow(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_long(a[1]), 0, 0));
}
inline JitValue _ns_math_sign(JitValue* a, int64_t) {
  auto x = _ns_adapt::take_long(a[0]);
  return _ns_adapt::v_long(x > 0 ? 1 : (x < 0 ? -1 : 0));
}
inline JitValue _ns_math_clamp(JitValue* a, int64_t) {
  auto x = _ns_adapt::take_long(a[0]);
  auto lo = _ns_adapt::take_long(a[1]);
  auto hi = _ns_adapt::take_long(a[2]);
  return _ns_adapt::v_long(x < lo ? lo : (x > hi ? hi : x));
}

// FS
inline JitValue _ns_fs_join(JitValue* a, int64_t n) {
  return _ns_adapt::v_string(culebra_runtime_fs_join(a, n, 0, 0));
}
inline JitValue _ns_fs_basename(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_fs_basename(_ns_adapt::take_str(a[0])));
}
inline JitValue _ns_fs_dirname(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_fs_dirname(_ns_adapt::take_str(a[0])));
}
inline JitValue _ns_fs_extension(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_fs_extension(_ns_adapt::take_str(a[0])));
}
inline JitValue _ns_fs_stem(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_fs_stem(_ns_adapt::take_str(a[0])));
}
inline JitValue _ns_fs_exists(JitValue* a, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_io_exists(_ns_adapt::take_str(a[0])));
}
inline JitValue _ns_fs_is_file(JitValue* a, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_fs_is_file(_ns_adapt::take_str(a[0])));
}
inline JitValue _ns_fs_is_dir(JitValue* a, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_fs_is_dir(_ns_adapt::take_str(a[0])));
}
inline JitValue _ns_fs_size(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_fs_size(_ns_adapt::take_str(a[0]), 0, 0));
}
inline JitValue _ns_fs_list_dir(JitValue* a, int64_t) {
  return _ns_adapt::v_array(
      culebra_runtime_fs_list_dir(_ns_adapt::take_str(a[0]), 0, 0));
}
inline JitValue _ns_fs_mkdir(JitValue* a, int64_t) {
  culebra_runtime_fs_mkdir(_ns_adapt::take_str(a[0]), 0, 0);
  return _ns_adapt::v_nil();
}
// remove(path, recursive=false): slab is full-arity via NsParamMeta.
inline JitValue _ns_fs_remove(JitValue* a, int64_t n) {
  JitValue rec = n > 1 ? a[1] : JitValue{TAG_BOOL, 0};
  if (rec.tag == TAG_BOOL && rec.data != 0) {
    culebra_runtime_fs_remove_all(_ns_adapt::take_str(a[0]), 0, 0);
  } else {
    culebra_runtime_fs_remove(_ns_adapt::take_str(a[0]), 0, 0);
  }
  return _ns_adapt::v_nil();
}
inline JitValue _ns_fs_stat(JitValue* a, int64_t) {
  return _ns_adapt::v_object(
      culebra_runtime_fs_stat(_ns_adapt::take_str(a[0]), 0, 0));
}
inline JitValue _ns_fs_rename(JitValue* a, int64_t) {
  culebra_runtime_fs_rename(_ns_adapt::take_str(a[0]),
                            _ns_adapt::take_str(a[1]), 0, 0);
  return _ns_adapt::v_nil();
}
// copy(src, dst, recursive=false): slab full-arity via NsParamMeta.
inline JitValue _ns_fs_copy(JitValue* a, int64_t n) {
  JitValue rec = n > 2 ? a[2] : JitValue{TAG_BOOL, 0};
  culebra_runtime_fs_copy(_ns_adapt::take_str(a[0]), _ns_adapt::take_str(a[1]),
                          rec.tag == TAG_BOOL && rec.data != 0 ? 1 : 0, 0, 0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_fs_normpath(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_fs_normpath(_ns_adapt::take_str(a[0])));
}
inline JitValue _ns_fs_is_abs(JitValue* a, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_fs_is_abs(_ns_adapt::take_str(a[0])));
}
inline JitValue _ns_fs_abspath(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_fs_abspath(_ns_adapt::take_str(a[0]), 0, 0));
}
inline JitValue _ns_fs_realpath(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_fs_realpath(_ns_adapt::take_str(a[0]), 0, 0));
}
inline JitValue _ns_fs_is_symlink(JitValue* a, int64_t) {
  return _ns_adapt::v_bool(
      culebra_runtime_fs_is_symlink(_ns_adapt::take_str(a[0])));
}
inline JitValue _ns_fs_symlink(JitValue* a, int64_t) {
  culebra_runtime_fs_symlink(_ns_adapt::take_str(a[0]),
                             _ns_adapt::take_str(a[1]), 0, 0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_fs_readlink(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_fs_readlink(_ns_adapt::take_str(a[0]), 0, 0));
}
inline JitValue _ns_fs_walk(JitValue* a, int64_t) {
  return _ns_adapt::v_array(
      culebra_runtime_fs_walk(_ns_adapt::take_str(a[0]), 0, 0));
}
inline JitValue _ns_fs_glob(JitValue* a, int64_t) {
  return _ns_adapt::v_array(
      culebra_runtime_fs_glob(_ns_adapt::take_str(a[0])));
}

// Random
inline JitValue _ns_random_seed(JitValue* a, int64_t) {
  culebra_runtime_random_seed(_ns_adapt::take_long(a[0]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_random_int(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_random_int(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_long(a[1]), 0, 0));
}
inline JitValue _ns_random_uniform(JitValue* a, int64_t) {
  return culebra_runtime_random_uniform(a[0].tag, a[0].data,
                                         a[1].tag, a[1].data);
}
inline JitValue _ns_random_gauss(JitValue* a, int64_t) {
  return culebra_runtime_random_gauss(a[0].tag, a[0].data,
                                       a[1].tag, a[1].data);
}
inline JitValue _ns_random_shuffle(JitValue* a, int64_t) {
  culebra_runtime_random_shuffle(_ns_adapt::take_array(a[0]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_random_weighted_choice(JitValue* a, int64_t) {
  return culebra_runtime_random_weighted_choice(
      _ns_adapt::take_array(a[0]), _ns_adapt::take_array(a[1]), 0, 0);
}

// Sys
inline JitValue _ns_sys_exit(JitValue* a, int64_t) {
  culebra_runtime_sys_exit(_ns_adapt::take_long(a[0]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_sys_env(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_sys_env(_ns_adapt::take_str(a[0])));
}
inline JitValue _ns_sys_time(JitValue*, int64_t) {
  return _ns_adapt::v_float(culebra_runtime_sys_time());
}

// GC.stat() — heap introspection. Returns an Object with `live_objects`
// (net live refcounted heap objects = births − frees, deterministic) and
// `heap_bytes` (those objects' struct sizes summed — element buffers and
// allocator overhead excluded, so it's a structural approximation). Snapshot
// the counters before allocating the result object so the container itself
// isn't included in its own report.
inline JitValue _ns_gc_stat(JitValue*, int64_t) {
  auto& gc = _gc_heap();
  gc.collect();  // report reachable objects, not registry residue not yet swept
  int64_t live = static_cast<int64_t>(gc.live_count());
  int64_t bytes = static_cast<int64_t>(gc.live_bytes());
  auto* obj = culebra_runtime_object_new();
  culebra_runtime_object_set(obj, "live_objects", /*mut=*/false, TAG_LONG,
                             live, 0, 0);
  culebra_runtime_object_set(obj, "heap_bytes", /*mut=*/false, TAG_LONG,
                             bytes, 0, 0);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(obj)};
}

// Proc. The first-class `Proc.*` value (bare-value calls) and AOT route
// through these trampoline adapters. They receive a positional slab whose
// slots match the NsParamMeta order (kwargs already resolved + defaults
// filled by culebra_runtime_call_with_kwargs); slots past `n` are treated
// as their defaults so a bare positional `p.run([cmd])` (no kwargs) still
// works.
namespace _proc_adapt {
// Read slab[i] when present; otherwise a nil sentinel.
inline JitValue at(JitValue* a, int64_t n, int64_t i) {
  return i < n ? a[i] : JitValue{TAG_NIL, 0};
}
// Optional cwd-style string slot: nil → absent.
inline bool str_slot(JitValue v, std::string& out) {
  if (v.tag == TAG_STRING || v.tag == TAG_STRINGVIEW) {
    out = _culebra_str_view(v.tag, v.data);
    return true;
  }
  return false;
}
// `env` Object slot → name/value pairs. nil → absent. Non-Object or
// non-String values raise TypeError.
inline bool env_slot(JitValue v,
                     std::vector<std::pair<std::string, std::string>>& out,
                     int64_t line, int64_t col) {
  if (v.tag != TAG_OBJECT) return false;
  if (!_ns_env_object_pairs(reinterpret_cast<JitObject*>(v.data), out)) {
    throw culebra::CulebraError("TypeError",
        std::format("Proc: env values must be String at {}:{}.", line, col),
        line, col);
  }
  return true;
}
}  // namespace _proc_adapt

inline JitValue _ns_proc_run(JitValue* a, int64_t n) {
  // slab: cmd, cwd, env, stdin, check, timeout
  std::string cwd_str;
  const std::string* cwd = _proc_adapt::str_slot(_proc_adapt::at(a, n, 1),
                                                 cwd_str) ? &cwd_str : nullptr;
  std::vector<std::pair<std::string, std::string>> over;
  const auto* env = _proc_adapt::env_slot(_proc_adapt::at(a, n, 2), over, 0, 0)
                        ? &over : nullptr;
  std::string stdin_data;
  _proc_adapt::str_slot(_proc_adapt::at(a, n, 3), stdin_data);
  JitValue chk = _proc_adapt::at(a, n, 4);
  bool check = chk.tag == TAG_BOOL && chk.data != 0;
  JitValue to = _proc_adapt::at(a, n, 5);
  int64_t timeout = to.tag == TAG_LONG ? to.data : 0;
  return _culebra_proc_run_impl(a[0].tag, a[0].data, cwd, env, stdin_data,
                                check, timeout, 0, 0);
}
inline JitValue _ns_proc_all(JitValue* a, int64_t n) {
  // slab: commands, limit, timeout, fail_fast, retries
  JitValue lim = _proc_adapt::at(a, n, 1);
  JitValue to = _proc_adapt::at(a, n, 2);
  JitValue ff = _proc_adapt::at(a, n, 3);
  JitValue rt = _proc_adapt::at(a, n, 4);
  return _culebra_proc_all_impl(
      a[0].tag, a[0].data, lim.tag == TAG_LONG ? lim.data : 0,
      to.tag == TAG_LONG ? to.data : 0, ff.tag == TAG_BOOL && ff.data != 0,
      rt.tag == TAG_LONG ? rt.data : 0, 0, 0);
}
inline JitValue _ns_proc_race(JitValue* a, int64_t) {
  return culebra_runtime_proc_race(a[0].tag, a[0].data, 0, 0);
}

// Isolate.spawn(fn, *args): a[0] = closure, a[1..] = positional args.
inline JitValue _ns_isolate_spawn(JitValue* a, int64_t n) {
  if (n < 1) {
    throw culebra::CulebraError("TypeError",
        "Isolate.spawn: first argument must be a function");
  }
  return culebra_jit_isolate_spawn(a[0].tag, a[0].data, n - 1, a + 1, 0, 0);
}

inline JitValue _ns_channel_new(JitValue* a, int64_t n) {
  return culebra_jit_channel_new(n, a, 0, 0);
}
inline JitValue _ns_channel_fan_in(JitValue* a, int64_t n) {
  return culebra_jit_channel_fan_in(n, a, 0, 0);
}

// Parallel.{map,each,map_settled,race}(items, fn, limit = 0). `limit` arrives in
// slab slot 2 — resolved from a positional arg or a `limit:` kwarg by the
// NsParamMeta hook (kParallelMeta), defaulting to 0 (= the cap). The four modes
// share one dispatch; the table needs a distinct fn pointer per method.
inline JitValue _ns_parallel_dispatch(JitValue* a, int64_t n,
                                      culebra::PMode mode) {
  if (n < 2) {
    throw culebra::CulebraError("TypeError",
        std::format("Parallel.{}: expected (items, fn)",
                    culebra::parallel_mode_name(mode)));
  }
  long limit = (n >= 3 && a[2].tag == TAG_LONG) ? a[2].data : 0;
  JitValue on_progress = (n >= 4) ? a[3] : JitValue{TAG_NIL, 0};
  return jit_parallel_run(a[0], a[1], limit, mode, 0, 0, on_progress);
}
inline JitValue _ns_parallel_map(JitValue* a, int64_t n) {
  return _ns_parallel_dispatch(a, n, culebra::PMode::Map);
}
inline JitValue _ns_parallel_each(JitValue* a, int64_t n) {
  return _ns_parallel_dispatch(a, n, culebra::PMode::Each);
}
inline JitValue _ns_parallel_map_settled(JitValue* a, int64_t n) {
  return _ns_parallel_dispatch(a, n, culebra::PMode::MapSettled);
}
inline JitValue _ns_parallel_race(JitValue* a, int64_t n) {
  return _ns_parallel_dispatch(a, n, culebra::PMode::Race);
}
inline JitValue _ns_proc_spawn(JitValue* a, int64_t n) {
  // slab: cmd, cwd, env, stdin
  std::string cwd_str;
  const std::string* cwd = _proc_adapt::str_slot(_proc_adapt::at(a, n, 1),
                                                 cwd_str) ? &cwd_str : nullptr;
  std::vector<std::pair<std::string, std::string>> over;
  const auto* env = _proc_adapt::env_slot(_proc_adapt::at(a, n, 2), over, 0, 0)
                        ? &over : nullptr;
  std::string stdin_data;
  _proc_adapt::str_slot(_proc_adapt::at(a, n, 3), stdin_data);
  return _culebra_proc_spawn_build(a[0].tag, a[0].data, cwd, env, stdin_data,
                                   0, 0);
}

// JSON. Slow path is positional-only; the kwargs form goes through
// compile_json_kwargs_adapter on the fast path.
inline JitValue _ns_json_stringify(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_json_stringify(a[0].tag, a[0].data, 0, false, 0));
}
inline JitValue _ns_json_parse(JitValue* a, int64_t) {
  return culebra_runtime_json_parse(_ns_adapt::take_str(a[0]),
                                     /*number_mode=*/"auto", /*lines=*/0);
}

#ifndef CULEBRA_RT_NO_TENSOR
// Tensor
inline JitValue _ns_tensor_zeros(JitValue* a, int64_t n) {
  return _ns_adapt::v_tensor(culebra_runtime_tensor_zeros(a, n, 0, 0));
}
inline JitValue _ns_tensor_ones(JitValue* a, int64_t n) {
  return _ns_adapt::v_tensor(culebra_runtime_tensor_ones(a, n, 0, 0));
}
inline JitValue _ns_tensor_randn(JitValue* a, int64_t n) {
  return _ns_adapt::v_tensor(culebra_runtime_tensor_randn(a, n, 0, 0));
}
inline JitValue _ns_tensor_from(JitValue* a, int64_t) {
  return _ns_adapt::v_tensor(
      culebra_runtime_tensor_from(_ns_adapt::take_array(a[0]), 0, 0));
}
inline JitValue _ns_tensor_from_csv(JitValue* a, int64_t) {
  return _ns_adapt::v_tensor(
      culebra_runtime_tensor_from_csv(_ns_adapt::take_str(a[0])));
}
// Activations relu/sigmoid/softmax are Tensor instance methods
// (`t.relu()`), dispatched in compile_builtin_method — not namespace
// functions.
inline JitValue _ns_tensor_eval(JitValue* a, int64_t n) {
  for (int64_t i = 0; i < n; i++) {
    if (auto* t = _ns_adapt::take_tensor(a[i])) {
      culebra_runtime_tensor_eval_one(t);
    }
  }
  return _ns_adapt::v_nil();
}
#endif  // CULEBRA_RT_NO_TENSOR

// --- The dispatch table ---

//===-- Regex: the `Regex` namespace functions. Like GC.stat, no fast-path
// branch or runtime helper is needed — these slow-path adapters do the work
// and build the JitObject/JitArray results directly. All take (pattern,
// subject, ...); flags are inline ((?i)/(?m)/(?s)). A Match is a data object
// { value, start, end, groups:[Group|nil], named:{name:Group} }; no-match nil.
//===------------------------------------------------------------------------//
inline std::shared_ptr<regexlib::Regex> _jit_regex_compile(const char* pat) {
  // Stateless cache keyed by pattern (own thread-local; see the /simplify note
  // about sharing with stdlib_interp's regex_compile_cached).
  static thread_local std::unordered_map<std::string,
                                         std::shared_ptr<regexlib::Regex>>
      cache;
  std::string p = pat ? pat : "";
  auto it = cache.find(p);
  if (it != cache.end()) return it->second;
  std::shared_ptr<regexlib::Regex> re;
  try {
    re = std::make_shared<regexlib::Regex>(p);
  } catch (const regexlib::RegexError& e) {
    throw culebra::CulebraError("RegexError",
                                std::format("Regex: {}", e.what()), 0, 0);
  }
  if (cache.size() > 256) cache.clear();
  cache.emplace(std::move(p), re);
  return re;
}

inline JitValue _jit_regex_group(const regexlib::Capture& c) {
  if (!c.matched) return _ns_adapt::v_nil();
  auto* g = culebra_runtime_object_new();
  culebra_runtime_object_set(g, "value", false, TAG_STRING,
                             reinterpret_cast<int64_t>(_culebra_heap_str(c.str)),
                             0, 0);
  culebra_runtime_object_set(g, "start", false, TAG_LONG,
                             static_cast<int64_t>(c.begin), 0, 0);
  culebra_runtime_object_set(g, "end", false, TAG_LONG,
                             static_cast<int64_t>(c.end), 0, 0);
  return _ns_adapt::v_object(g);
}

inline JitValue _jit_regex_match(const regexlib::MatchResult& m) {
  auto* o = culebra_runtime_object_new();
  culebra_runtime_object_set(o, "value", false, TAG_STRING,
                             reinterpret_cast<int64_t>(_culebra_heap_str(m.str)),
                             0, 0);
  culebra_runtime_object_set(o, "start", false, TAG_LONG,
                             static_cast<int64_t>(m.begin), 0, 0);
  culebra_runtime_object_set(o, "end", false, TAG_LONG,
                             static_cast<int64_t>(m.end), 0, 0);
  auto* groups = culebra_runtime_array_new();
  for (const auto& g : m.groups) {
    auto gv = _jit_regex_group(g);
    culebra_runtime_array_push(groups, gv.tag, gv.data);
  }
  culebra_runtime_object_set(o, "groups", false, TAG_ARRAY,
                             reinterpret_cast<int64_t>(groups), 0, 0);
  auto* named = culebra_runtime_object_new();
  for (const auto& kv : m.named) {
    auto gv = _jit_regex_group(m.group(kv.second));
    culebra_runtime_object_set(named, kv.first.c_str(), false, gv.tag, gv.data,
                               0, 0);
  }
  culebra_runtime_object_set(o, "named", false, TAG_OBJECT,
                             reinterpret_cast<int64_t>(named), 0, 0);
  return _ns_adapt::v_object(o);
}

inline JitValue _ns_regex_check(JitValue* a, int64_t) {
  _jit_regex_compile(_ns_adapt::take_str(a[0]));  // validate; throws on bad pattern
  return _ns_adapt::v_nil();
}
inline JitValue _ns_regex_test(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::take_str(a[0]));
  return _ns_adapt::v_bool(re->test(_ns_adapt::take_str(a[1])));
}
inline JitValue _ns_regex_find(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::take_str(a[0]));
  auto m = re->search(_ns_adapt::take_str(a[1]));
  return m.matched ? _jit_regex_match(m) : _ns_adapt::v_nil();
}
inline JitValue _ns_regex_match(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::take_str(a[0]));
  auto m = re->match(_ns_adapt::take_str(a[1]));
  return m.matched ? _jit_regex_match(m) : _ns_adapt::v_nil();
}
inline JitValue _ns_regex_find_all(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::take_str(a[0]));
  auto* arr = culebra_runtime_array_new();
  for (auto& m : re->find_all(_ns_adapt::take_str(a[1]))) {
    auto mv = _jit_regex_match(m);
    culebra_runtime_array_push(arr, mv.tag, mv.data);
  }
  return _ns_adapt::v_array(arr);
}
// find_all_str(pattern, s) -> [String]: matched texts only, no Match objects
// (the per-match Object structure dominated match-dense workloads). See interp.
inline JitValue _ns_regex_find_all_str(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::take_str(a[0]));
  auto* arr = culebra_runtime_array_new();
  for (auto& m : re->find_all(_ns_adapt::take_str(a[1]))) {
    culebra_runtime_array_push(
        arr, TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(m.str)));
  }
  return _ns_adapt::v_array(arr);
}
// count(pattern, s) -> Long: number of non-overlapping matches, no objects.
inline JitValue _ns_regex_count(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::take_str(a[0]));
  return JitValue{TAG_LONG, static_cast<int64_t>(
                                re->find_all(_ns_adapt::take_str(a[1])).size())};
}
// find_all_index(pattern, s) -> [Int]: flat byte spans [s0,e0,s1,e1,...].
// Longs are inline in the JitArray, so the whole result is one allocation.
inline JitValue _ns_regex_find_all_index(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::take_str(a[0]));
  auto* arr = culebra_runtime_array_new();
  for (auto& m : re->find_all(_ns_adapt::take_str(a[1]))) {
    culebra_runtime_array_push(arr, TAG_LONG, static_cast<int64_t>(m.begin));
    culebra_runtime_array_push(arr, TAG_LONG, static_cast<int64_t>(m.end));
  }
  return _ns_adapt::v_array(arr);
}
inline JitValue _ns_regex_replace_all(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::take_str(a[0]));
  std::string out =
      re->replace_all(_ns_adapt::take_str(a[1]), _ns_adapt::take_str(a[2]));
  return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(out))};
}
// find_from(pattern, s, pos) -> { m: Match|nil, next: Int }. See the interp
// twin in stdlib_interp.h: leftmost match at/after byte `pos` with absolute
// offsets, plus the grapheme-correct resume position.
inline JitValue _ns_regex_find_from(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::take_str(a[0]));
  std::string s = _ns_adapt::take_str(a[1]);
  long pos = static_cast<long>(a[2].data);
  auto* out = culebra_runtime_object_new();
  auto miss = [&](long nxt) {
    culebra_runtime_object_set(out, "m", false, TAG_NIL, 0, 0, 0);
    culebra_runtime_object_set(out, "nxt", false, TAG_LONG,
                               static_cast<int64_t>(nxt), 0, 0);
    return _ns_adapt::v_object(out);
  };
  if (pos < 0 || static_cast<size_t>(pos) > s.size())
    return miss(static_cast<long>(s.size()) + 1);
  std::string_view suffix = std::string_view(s).substr(pos);
  auto m = re->search(suffix);
  if (!m.matched) return miss(static_cast<long>(s.size()) + 1);
  long next;
  if (m.end > m.begin) {
    next = pos + static_cast<long>(m.end);
  } else {
    auto seg = regexlib::detail::segment(suffix);
    size_t gi = static_cast<size_t>(m.end_grapheme) + 1;
    size_t nb =
        gi < seg.byte_begin.size() ? seg.byte_begin[gi] : suffix.size() + 1;
    next = pos + static_cast<long>(nb);
  }
  m.begin += pos;
  m.end += pos;
  for (auto& g : m.groups)
    if (g.matched) { g.begin += pos; g.end += pos; }
  auto mv = _jit_regex_match(m);
  culebra_runtime_object_set(out, "m", false, mv.tag, mv.data, 0, 0);
  culebra_runtime_object_set(out, "nxt", false, TAG_LONG,
                             static_cast<int64_t>(next), 0, 0);
  return _ns_adapt::v_object(out);
}
inline JitValue _ns_regex_split(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::take_str(a[0]));
  std::string s = _ns_adapt::take_str(a[1]);
  auto* arr = culebra_runtime_array_new();
  size_t cursor = 0;
  for (auto& m : re->find_all(s)) {
    auto piece = s.substr(cursor, m.begin - cursor);
    culebra_runtime_array_push(
        arr, TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(piece)));
    cursor = m.end;
  }
  auto last = s.substr(cursor);
  culebra_runtime_array_push(
      arr, TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(last)));
  return _ns_adapt::v_array(arr);
}

// Per-parameter metadata for stdlib methods that accept kwargs / have
// defaults. Defaults are produced by a factory fn (mirrors kNsConstants'
// late-bound `build`) so heap values like "" can be materialized fresh
// per call. Methods without kwargs leave NsMethod::params null.
struct NsParam {
  const char* name;
  bool has_default;
  bool kw_only;
  JitValue (*make_default)();  // null when has_default == false
};

struct NsParamMeta {
  const NsParam* params;
  int n_params;
  int kwargs_rest_idx;    // -1 = none
  int first_kw_only_idx;  // -1 = none
};

inline JitValue _ns_def_nil()   { return {TAG_NIL, 0}; }
inline JitValue _ns_def_false() { return {TAG_BOOL, 0}; }
inline JitValue _ns_def_zero()  { return {TAG_LONG, 0}; }
inline JitValue _ns_def_empty_str() {
  return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(""))};
}
inline JitValue _ns_def_mode_r() {
  return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str("r"))};
}

struct NsMethod {
  const char* ns;
  const char* name;
  int8_t arity;  // -1 = variadic
  JitValue (*adapter)(JitValue* args, int64_t n);
  const NsParamMeta* params = nullptr;  // null = all-positional, no defaults
};

// Param metadata for the kwarg-accepting Proc methods. Names/defaults
// mirror make_proc_namespace() in stdlib_interp.h; _check_ns_drift_once
// verifies the two stay in sync (debug builds).
inline const NsParam kProcRunParams[] = {
  {"cmd",     false, false, nullptr},
  {"cwd",     true,  false, &_ns_def_nil},
  {"env",     true,  false, &_ns_def_nil},
  {"stdin",   true,  false, &_ns_def_empty_str},
  {"check",   true,  false, &_ns_def_false},
  {"timeout", true,  false, &_ns_def_zero},
};
inline const NsParamMeta kProcRunMeta = {kProcRunParams, 6, -1, -1};

inline const NsParam kProcAllParams[] = {
  {"commands",  false, false, nullptr},
  {"limit",     true,  false, &_ns_def_zero},
  {"timeout",   true,  false, &_ns_def_zero},
  {"fail_fast", true,  false, &_ns_def_false},
  {"retries",   true,  false, &_ns_def_zero},
};
inline const NsParamMeta kProcAllMeta = {kProcAllParams, 5, -1, -1};

inline const NsParam kProcSpawnParams[] = {
  {"cmd",   false, false, nullptr},
  {"cwd",   true,  false, &_ns_def_nil},
  {"env",   true,  false, &_ns_def_nil},
  {"stdin", true,  false, &_ns_def_empty_str},
};
inline const NsParamMeta kProcSpawnMeta = {kProcSpawnParams, 4, -1, -1};

// FS.remove(path, recursive=false) / FS.copy(src, dst, recursive=false).
// Names/defaults mirror make_fs_namespace in stdlib_interp.h.
inline const NsParam kFsRemoveParams[] = {
  {"path",      false, false, nullptr},
  {"recursive", true,  false, &_ns_def_false},
};
inline const NsParamMeta kFsRemoveMeta = {kFsRemoveParams, 2, -1, -1};

inline const NsParam kFsCopyParams[] = {
  {"src",       false, false, nullptr},
  {"dst",       false, false, nullptr},
  {"recursive", true,  false, &_ns_def_false},
};
inline const NsParamMeta kFsCopyMeta = {kFsCopyParams, 3, -1, -1};

// File.open(path, mode="r") / File.with(path, mode="r", fn).
inline const NsParam kFileOpenParams[] = {
  {"path", false, false, nullptr},
  {"mode", true,  false, &_ns_def_mode_r},
};
inline const NsParamMeta kFileOpenMeta = {kFileOpenParams, 2, -1, -1};

inline const NsParam kFileWithParams[] = {
  {"path", false, false, nullptr},
  {"mode", true,  false, &_ns_def_mode_r},
  {"fn",   false, false, nullptr},
};
inline const NsParamMeta kFileWithMeta = {kFileWithParams, 3, -1, -1};

// Parallel.{map,each,...}(items, fn, limit=0, on_progress=nil). Mirrors
// make_parallel_namespace in isolate.h; the NsParamMeta hook resolves `limit:`
// and `on_progress:` kwargs into slab slots 2 and 3.
inline const NsParam kParallelParams[] = {
  {"items",       false, false, nullptr},
  {"fn",          false, false, nullptr},
  {"limit",       true,  false, &_ns_def_zero},
  {"on_progress", true,  false, &_ns_def_nil},
};
inline const NsParamMeta kParallelMeta = {kParallelParams, 4, -1, -1};

inline const NsMethod kNsMethods[] = {
  {"IO",     "puts",      1, &_ns_io_puts},
  {"IO",     "print",     1, &_ns_io_print},
  {"IO",     "input",     0, &_ns_io_input},

  {"Math",   "abs",       1, &_ns_math_abs},
  {"Math",   "log",       1, &_ns_math_log},
  {"Math",   "exp",       1, &_ns_math_exp},
  {"Math",   "sqrt",      1, &_ns_math_sqrt},
  {"Math",   "floor",     1, &_ns_math_floor},
  {"Math",   "ceil",      1, &_ns_math_ceil},
  {"Math",   "round",     1, &_ns_math_round},
  {"Math",   "min",      -1, &_ns_math_min},
  {"Math",   "max",      -1, &_ns_math_max},
  {"Math",   "pow",       2, &_ns_math_pow},
  {"Math",   "sign",      1, &_ns_math_sign},
  {"Math",   "clamp",     3, &_ns_math_clamp},

  {"FS",     "join",     -1, &_ns_fs_join},
  {"FS",     "basename",  1, &_ns_fs_basename},
  {"FS",     "dirname",   1, &_ns_fs_dirname},
  {"FS",     "extension", 1, &_ns_fs_extension},
  {"FS",     "stem",      1, &_ns_fs_stem},
  {"FS",     "exists",    1, &_ns_fs_exists},
  {"FS",     "is_file",   1, &_ns_fs_is_file},
  {"FS",     "is_dir",    1, &_ns_fs_is_dir},
  {"FS",     "read",      1, &_ns_fs_read},
  {"FS",     "write",     2, &_ns_fs_write},
  {"FS",     "size",      1, &_ns_fs_size},
  {"FS",     "list_dir",  1, &_ns_fs_list_dir},
  {"FS",     "mkdir",     1, &_ns_fs_mkdir},
  {"FS",     "remove",    1, &_ns_fs_remove, &kFsRemoveMeta},
  {"FS",     "stat",      1, &_ns_fs_stat},
  {"FS",     "rename",    2, &_ns_fs_rename},
  {"FS",     "copy",      2, &_ns_fs_copy, &kFsCopyMeta},
  {"FS",     "normpath",  1, &_ns_fs_normpath},
  {"FS",     "is_abs",    1, &_ns_fs_is_abs},
  {"FS",     "abspath",   1, &_ns_fs_abspath},
  {"FS",     "realpath",  1, &_ns_fs_realpath},
  {"FS",     "is_symlink",1, &_ns_fs_is_symlink},
  {"FS",     "symlink",   2, &_ns_fs_symlink},
  {"FS",     "readlink",  1, &_ns_fs_readlink},
  {"FS",     "walk",      1, &_ns_fs_walk},
  {"FS",     "glob",      1, &_ns_fs_glob},

  {"File",   "open",      1, &_ns_file_open, &kFileOpenMeta},
  {"File",   "with",      2, &_ns_file_with, &kFileWithMeta},

  {"Random", "seed",            1, &_ns_random_seed},
  {"Random", "int",             2, &_ns_random_int},
  {"Random", "uniform",         2, &_ns_random_uniform},
  {"Random", "gauss",           2, &_ns_random_gauss},
  {"Random", "shuffle",         1, &_ns_random_shuffle},
  {"Random", "weighted_choice", 2, &_ns_random_weighted_choice},

  {"Sys",    "exit", 1, &_ns_sys_exit},
  {"Sys",    "env",  1, &_ns_sys_env},
  {"Sys",    "time", 0, &_ns_sys_time},

  {"GC",     "stat", 0, &_ns_gc_stat},

  {"_Regex", "check",       1, &_ns_regex_check},
  {"_Regex", "test",        2, &_ns_regex_test},
  {"_Regex", "find",        2, &_ns_regex_find},
  {"_Regex", "match",       2, &_ns_regex_match},
  {"_Regex", "find_from",   3, &_ns_regex_find_from},
  {"_Regex", "find_all",    2, &_ns_regex_find_all},
  {"_Regex", "find_all_str",2, &_ns_regex_find_all_str},
  {"_Regex", "find_all_index",2, &_ns_regex_find_all_index},
  {"_Regex", "count",       2, &_ns_regex_count},
  {"_Regex", "replace_all", 3, &_ns_regex_replace_all},
  {"_Regex", "split",       2, &_ns_regex_split},

  {"Proc",   "run",   1, &_ns_proc_run,   &kProcRunMeta},
  {"Proc",   "all",   1, &_ns_proc_all,   &kProcAllMeta},
  {"Proc",   "race",  1, &_ns_proc_race},
  {"Proc",   "spawn", 1, &_ns_proc_spawn, &kProcSpawnMeta},

  {"Isolate", "spawn", -1, &_ns_isolate_spawn},
  {"Channel", "new",    -1, &_ns_channel_new},
  {"Channel", "fan_in",  1, &_ns_channel_fan_in},
  {"Parallel", "map",         2, &_ns_parallel_map,         &kParallelMeta},
  {"Parallel", "each",        2, &_ns_parallel_each,        &kParallelMeta},
  {"Parallel", "map_settled", 2, &_ns_parallel_map_settled, &kParallelMeta},
  {"Parallel", "race",        2, &_ns_parallel_race,        &kParallelMeta},

  {"JSON",   "stringify", 1, &_ns_json_stringify},
  {"JSON",   "parse",     1, &_ns_json_parse},

#ifndef CULEBRA_RT_NO_TENSOR
  {"Tensor", "zeros",    -1, &_ns_tensor_zeros},
  {"Tensor", "ones",     -1, &_ns_tensor_ones},
  {"Tensor", "randn",    -1, &_ns_tensor_randn},
  {"Tensor", "from",      1, &_ns_tensor_from},
  {"Tensor", "from_csv",  1, &_ns_tensor_from_csv},
  {"Tensor", "eval",     -1, &_ns_tensor_eval},
#endif
};

// Namespace-level constants (Math.pi, etc). Slot values are immutable
// at the language level; we register them once when building the
// namespace object. Sys.argv is built dynamically (process arg list)
// — see `_jit_build_namespace_object`.
struct NsConstant {
  const char* ns;
  const char* name;
  JitValue (*build)();  // late-bound so M_PI etc. evaluate at build time
};

inline JitValue _ns_const_pi()  { return _ns_adapt::v_float(M_PI); }
inline JitValue _ns_const_e()   { return _ns_adapt::v_float(M_E); }
inline JitValue _ns_const_inf() {
  return _ns_adapt::v_float(std::numeric_limits<double>::infinity());
}
inline JitValue _ns_const_nan() {
  return _ns_adapt::v_float(std::numeric_limits<double>::quiet_NaN());
}

inline const NsConstant kNsConstants[] = {
  {"Math", "pi",  &_ns_const_pi},
  {"Math", "e",   &_ns_const_e},
  {"Math", "inf", &_ns_const_inf},
  {"Math", "nan", &_ns_const_nan},
};

// --- Trampoline + factory + lazy table ---

inline JitValue _jit_ns_method_trampoline(
    JitClosure* cls, JitValue /*this_val*/, int64_t n_args, JitValue* args) {
  const auto* m = reinterpret_cast<const NsMethod*>(
      cls->captures[0]->value.data);
  auto release_args = [&]() {
    for (int64_t i = 0; i < n_args; i++) {
      _culebra_value_release_impl(args[i].tag, args[i].data);
    }
  };
  // Methods with NsParamMeta accept a variable arity (required params up to
  // the full param count); the kwarg resolver fills a full-arity slab and
  // bare positional calls pass a prefix. Methods without params keep the
  // strict fixed-arity check.
  if (m->params) {
    int64_t n_required = 0;
    for (int i = 0; i < m->params->n_params; i++) {
      if (!m->params->params[i].has_default) n_required++;
    }
    if (n_args < n_required || n_args > m->params->n_params) {
      release_args();
      _ns_adapt::arity_error(m->ns, m->name, n_required, n_args);
    }
  } else if (m->arity >= 0 && n_args != m->arity) {
    release_args();
    _ns_adapt::arity_error(m->ns, m->name, m->arity, n_args);
  }
  try {
    auto r = m->adapter(args, n_args);
    release_args();
    return r;
  } catch (...) {
    release_args();
    throw;
  }
}

inline JitClosure* _jit_make_ns_method_closure(const NsMethod* m) {
  auto* cls = new JitClosure();
  cls->refcount = 1;
  cls->fn_ptr = reinterpret_cast<void*>(_jit_ns_method_trampoline);
  cls->n_captures = 1;
  cls->captures = new JitCell*[1];
  cls->captures[0] = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(m));
  cls->arity = m->arity < 0 ? 0 : static_cast<size_t>(m->arity);
  _gc_register(cls, GC_TAG_FUNC);
  return cls;
}

// Resolve a kwarg/splat call against an ns-method closure carrying
// NsParamMeta. Mirrors culebra_runtime_call_with_kwargs' merge order
// (splat first, explicit kwargs override) but fills missing defaulted
// slots with the param's real default value (the C++ adapters have no
// callee prologue to expand a TAG_UNFILLED sentinel). Builds a
// full-arity positional slab and dispatches through the trampoline.
inline bool _jit_ns_kwarg_resolve(
    JitClosure* cls, JitValue this_val, int64_t n_pos, JitValue* positional,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs, int64_t line, int64_t col,
    JitValue* out) {
  if (cls->fn_ptr != reinterpret_cast<void*>(_jit_ns_method_trampoline)) {
    return false;
  }
  const auto* m = reinterpret_cast<const NsMethod*>(
      cls->captures[0]->value.data);
  if (!m->params) return false;  // method doesn't take kwargs
  const NsParamMeta* pm = m->params;

  auto release_all = [&]() {
    for (int64_t i = 0; i < n_pos; i++)
      _culebra_value_release_impl(positional[i].tag, positional[i].data);
    for (int64_t i = 0; i < n_kw; i++)
      _culebra_value_release_impl(kw_vals[i].tag, kw_vals[i].data);
    for (int64_t i = 0; i < n_splat; i++)
      _culebra_value_release_impl(splat_objs[i].tag, splat_objs[i].data);
  };

  // Merge splats then explicit kwargs (each owns +1 in `merged`).
  std::unordered_map<std::string_view, JitValue> merged;
  for (int64_t i = 0; i < n_splat; i++) {
    if (splat_objs[i].tag != TAG_OBJECT) {
      release_all();
      throw culebra::CulebraError("TypeError",
          std::format("**: splat operand must be Object at {}:{}.", line, col),
          line, col);
    }
    auto* obj = reinterpret_cast<JitObject*>(splat_objs[i].data);
    if (obj->shape) {
      for (size_t k = 0; k < obj->shape->names.size(); k++) {
        auto& sv = obj->slots[k].value;
        culebra_runtime_value_retain(sv.tag, sv.data);
        auto it = merged.find(obj->shape->names[k]);
        if (it != merged.end()) {
          _culebra_value_release_impl(it->second.tag, it->second.data);
          it->second = sv;
        } else {
          merged.emplace(obj->shape->names[k], sv);
        }
      }
    }
  }
  for (int64_t i = 0; i < n_kw; i++) {
    auto it = merged.find(kw_keys[i]);
    if (it != merged.end()) {
      _culebra_value_release_impl(it->second.tag, it->second.data);
      it->second = kw_vals[i];
    } else {
      merged.emplace(kw_keys[i], kw_vals[i]);
    }
  }

  int n = pm->n_params;
  std::vector<JitValue> slab(n);
  std::vector<bool> filled(n, false);
  // Positional args bind leftmost params; reject if also given by keyword.
  for (int64_t i = 0; i < n_pos && i < n; i++) {
    if (merged.count(pm->params[i].name)) {
      for (int64_t k = 0; k < i; k++)
        _culebra_value_release_impl(slab[k].tag, slab[k].data);
      for (int64_t k = i; k < n_pos; k++)
        _culebra_value_release_impl(positional[k].tag, positional[k].data);
      for (auto& [_, v] : merged)
        _culebra_value_release_impl(v.tag, v.data);
      throw culebra::CulebraError("TypeError",
          std::format("got argument '{}' both positionally and as a "
                      "keyword at {}:{}.", pm->params[i].name, line, col),
          line, col);
    }
    slab[i] = positional[i];
    filled[i] = true;
  }
  if (n_pos > n) {  // too many positionals
    for (int64_t k = 0; k < n; k++)
      _culebra_value_release_impl(slab[k].tag, slab[k].data);
    for (int64_t k = n; k < n_pos; k++)
      _culebra_value_release_impl(positional[k].tag, positional[k].data);
    for (auto& [_, v] : merged)
      _culebra_value_release_impl(v.tag, v.data);
    _ns_adapt::arity_error(m->ns, m->name, n, n_pos);
  }
  // Remaining params: from merged kwargs, else default, else ArityError.
  for (int i = static_cast<int>(n_pos); i < n; i++) {
    auto it = merged.find(pm->params[i].name);
    if (it != merged.end()) {
      slab[i] = it->second;
      filled[i] = true;
      merged.erase(it);
    } else if (pm->params[i].has_default) {
      slab[i] = pm->params[i].make_default();
      filled[i] = true;
    } else {
      for (int k = 0; k < n; k++)
        if (filled[k]) _culebra_value_release_impl(slab[k].tag, slab[k].data);
      for (auto& [_, v] : merged)
        _culebra_value_release_impl(v.tag, v.data);
      throw culebra::CulebraError("ArityError",
          std::format("missing required argument '{}' at {}:{}.",
                      pm->params[i].name, line, col), line, col);
    }
  }
  // Any leftover kwargs are unknown to this method.
  if (!merged.empty()) {
    auto bad = std::string(merged.begin()->first);
    for (int k = 0; k < n; k++)
      _culebra_value_release_impl(slab[k].tag, slab[k].data);
    for (auto& [_, v] : merged)
      _culebra_value_release_impl(v.tag, v.data);
    throw culebra::CulebraError("TypeError",
        std::format("unknown keyword argument '{}' at {}:{}.", bad, line, col),
        line, col);
  }

  // Dispatch through the trampoline (it releases the slab values).
  *out = _jit_ns_method_trampoline(cls, this_val, n, slab.data());
  return true;
}

// Install the kwarg hook once, before any JIT call runs.
inline const bool _jit_ns_kwarg_hook_installed = [] {
  _jit_ns_kwarg_hook = &_jit_ns_kwarg_resolve;
  return true;
}();

inline JitObject* _jit_build_namespace_object(std::string_view ns_name) {
  auto* obj = culebra_runtime_object_new();
  for (auto& m : kNsMethods) {
    if (ns_name != m.ns) continue;
    auto* fn = _jit_make_ns_method_closure(&m);
    obj->append_slot(m.name,
                     JitValue{TAG_FUNC, reinterpret_cast<int64_t>(fn)},
                     /*mut=*/false);
  }
  for (auto& c : kNsConstants) {
    if (ns_name != c.ns) continue;
    obj->append_slot(c.name, c.build(), /*mut=*/false);
  }
  if (ns_name == "Sys") {
    auto* a = culebra_runtime_sys_argv();
    obj->append_slot(
        "argv", JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(a)},
        /*mut=*/false);
  }
  return obj;
}

struct _JitNamespaceTable {
  std::unordered_map<std::string, JitObject*> entries;
  ~_JitNamespaceTable() {
    for (auto& [_, obj] : entries) {
      _culebra_value_release_impl(TAG_OBJECT,
                                  reinterpret_cast<int64_t>(obj));
    }
  }
};

// Namespace names this dispatcher knows how to build. Kept in sync
// with interp's `setup_built_in_functions` by the drift check below.
// `_Time` is the Time-class ABI primitive — internal, not user-facing
// — and intentionally excluded.
inline bool _is_known_ns(std::string_view name) {
  for (auto& m : kNsMethods) if (name == m.ns) return true;
  for (auto& c : kNsConstants) if (name == c.ns) return true;
  return name == "Sys";  // Sys has only constants in some configs
}

inline JitObject* _jit_namespace_get_or_build(const std::string& name) {
  auto& table = culebra::runtime_substate<_JitNamespaceTable>(
                    culebra::kSlotJitNamespaceTable).entries;
  auto it = table.find(name);
  if (it != table.end()) return it->second;
  if (!_is_known_ns(name)) return nullptr;
  auto* obj = _jit_build_namespace_object(name);
  table.emplace(name, obj);
  // Cached for the program's lifetime and reached only through this table
  // (off any scanned stack between uses), so pin it as a permanent root; the
  // marker traces its method closures as children.
  _gc_heap().pin(obj);
  return obj;
}

#ifndef NDEBUG
// One-shot drift detector: if interp's setup_built_in_functions
// binds a namespace this dispatcher doesn't know about, abort the
// process with a clear message at first slow-path resolve. Catches
// the case where someone adds a new stdlib namespace (e.g. `Net`)
// to stdlib_interp.h and forgets to update kNsMethods here.
inline void _check_ns_drift_once() {
  static const bool checked = []() {
    static const std::set<std::string_view> kInternalNs = {"_Time"};
    auto env = culebra::environment();
    for (const auto& [key, sym] : env->dictionary) {
      if (sym.val.type != culebra::Value::Object) continue;
      std::string_view n(key);
      if (kInternalNs.contains(n)) continue;
      if (!_is_known_ns(n)) {
        std::fprintf(stderr,
            "culebra: JIT namespace drift — interp binds '%s' but "
            "stdlib_jit.h::kNsMethods does not cover it. Add adapters "
            "and table rows for it.\n",
            std::string(n).c_str());
        std::abort();
      }
    }
    // For methods carrying NsParamMeta (kwarg/default support), verify the
    // JIT-side param names + default flags match the interp's FunctionValue
    // definition. Catches a one-sided signature edit (e.g. adding a kwarg to
    // make_proc_namespace but forgetting the kProc*Params table).
    auto fail = [](const NsMethod& m, const std::string& why) {
      std::fprintf(stderr,
          "culebra: JIT param-meta drift for %s.%s — %s. Sync the "
          "kNsMethods NsParamMeta with make_%s_namespace in "
          "stdlib_interp.h.\n",
          m.ns, m.name, why.c_str(), m.ns);
      std::abort();
    };
    for (const auto& m : kNsMethods) {
      if (!m.params) continue;
      auto ns_it = env->dictionary.find(m.ns);
      if (ns_it == env->dictionary.end() ||
          ns_it->second.val.type != culebra::Value::Object) {
        fail(m, "interp namespace not found");
      }
      const auto& ns_obj = ns_it->second.val.to_object();
      if (!ns_obj.has(m.name)) fail(m, "interp method not found");
      const auto& fn_val = ns_obj.get(m.name);
      if (fn_val.type != culebra::Value::Function) fail(m, "not a Function");
      const auto& params = *fn_val.to_function().params;
      if (static_cast<int>(params.size()) != m.params->n_params) {
        fail(m, std::format("param count {} != NsParamMeta {}",
                            params.size(), m.params->n_params));
      }
      for (int i = 0; i < m.params->n_params; i++) {
        const auto& ip = params[i];
        const auto& jp = m.params->params[i];
        if (ip.name != jp.name) {
          fail(m, std::format("param {} name '{}' != '{}'", i,
                              std::string(ip.name), jp.name));
        }
        bool ip_has_default =
            ip.default_expr != nullptr || ip.default_value != nullptr;
        if (ip_has_default != jp.has_default) {
          fail(m, std::format("param '{}' default flag mismatch", jp.name));
        }
      }
    }
    return true;
  }();
  (void)checked;
}
#else
inline void _check_ns_drift_once() {}
#endif

extern "C" {
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_namespace_get(const char* name,
                               int8_t* out_tag, int64_t* out_data) {
  _check_ns_drift_once();
  auto* obj = _jit_namespace_get_or_build(std::string(name ? name : ""));
  if (!obj) {
    culebra::throw_runtime_error_at(
        "NameError",
        std::format("undefined variable '{}'...", name ? name : ""),
        0, 0);
  }
  culebra_runtime_value_retain(TAG_OBJECT,
                                reinterpret_cast<int64_t>(obj));
  *out_tag = TAG_OBJECT;
  *out_data = reinterpret_cast<int64_t>(obj);
}
}  // extern "C"

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
  // `hash(v)` builtin: (tag, data, line, col) -> int64. The Object
  // path inside the runtime invokes a user `hash()` method; primitives
  // share JitValueHash with the AnyKeyMap.
  jit.module_->getOrInsertFunction(rt::hash_any, jit.builder_.getInt64Ty(),
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
  for (auto name : {rt::fs_mkdir, rt::fs_remove, rt::fs_remove_all}) {
    jit.module_->getOrInsertFunction(name, jit.builder_.getVoidTy(), ptrTy,
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty());
  }
  // FS extensions. (ptr)->i64 / (ptr)->ptr query+pure helpers.
  for (auto name : {rt::fs_is_abs, rt::fs_is_symlink}) {
    jit.module_->getOrInsertFunction(name, jit.builder_.getInt64Ty(), ptrTy);
  }
  jit.module_->getOrInsertFunction(rt::fs_normpath, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::fs_glob, ptrTy, ptrTy);
  // (path, line, col) -> ptr (string or Object* or Array*), throws on error.
  for (auto name : {rt::fs_abspath, rt::fs_realpath, rt::fs_readlink,
                    rt::fs_stat, rt::fs_walk}) {
    jit.module_->getOrInsertFunction(name, ptrTy, ptrTy,
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty());
  }
  // (src, dst, line, col) -> void.
  for (auto name : {rt::fs_rename, rt::fs_symlink}) {
    jit.module_->getOrInsertFunction(name, jit.builder_.getVoidTy(), ptrTy,
                                 ptrTy, jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty());
  }
  // FS.copy(src, dst, recursive, line, col) -> void.
  jit.module_->getOrInsertFunction(rt::fs_copy, jit.builder_.getVoidTy(),
                               ptrTy, ptrTy, jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  // _Time (Long-nanos primitives + Float monotonic / sleep).
  {
  auto i64 = jit.builder_.getInt64Ty();
  jit.module_->getOrInsertFunction(rt::time_monotonic, jit.builder_.getDoubleTy());
  jit.module_->getOrInsertFunction(rt::time_sleep, jit.builder_.getVoidTy(),
                                   jit.builder_.getDoubleTy());
  jit.module_->getOrInsertFunction(rt::time_now_nanos, i64);
  jit.module_->getOrInsertFunction(rt::time_from_iso_nanos, i64,
                                   ptrTy, i64, i64);
  jit.module_->getOrInsertFunction(rt::time_parse_nanos, i64,
                                   ptrTy, ptrTy, i64, i64);
  jit.module_->getOrInsertFunction(rt::time_iso_nanos, ptrTy, i64, i64);
  jit.module_->getOrInsertFunction(rt::time_format_nanos, ptrTy,
                                   i64, ptrTy, i64);
  jit.module_->getOrInsertFunction(rt::time_weekday_nanos, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::time_parts_nanos, ptrTy, i64, i64);
  jit.module_->getOrInsertFunction(rt::time_from_parts_nanos, i64,
                                   ptrTy, i64);
  jit.module_->getOrInsertFunction(rt::time_add_nanos, i64,
                                   i64, i64, i64, i64, i64, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::time_start_of_nanos, i64,
                                   i64, ptrTy, i64, i64, i64);
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
  // (has_next_cls, next_cls, iter_tag, iter_data, &out_tag, &out_data) -> i64 (1/0)
  jit.module_->getOrInsertFunction(rt::iter_advance, i64,
                               ptrTy, ptrTy, i8, i64, ptrTy, ptrTy);
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
  // Global builtins (puts, to_long, etc.) parse positionally. None of
  // them accept kwargs today; if the call carries kwargs/splats and
  // matches a built-in name surface a clean SyntaxError. If the user
  // shadowed the name with their own `let X = fn (...) {...}` binding,
  // defer to the downstream user-fn resolver (mirrors interp's
  // name-resolution-first behavior). Otherwise return nullptr so
  // downstream call dispatch can take over.
  if (!JIT::arg_list_is_positional_only(argsAst)) {
    if (jit.lookup_fn_ast(name) != nullptr) return nullptr;
    static const std::set<std::string_view> known_globals = {
        "puts", "print", "to_long", "to_float",
        "to_string", "type_of", "iota", "range", "hash",
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

  if (name == "hash" && argsAst.nodes.size() == 1) {
    auto arg = jit.compile(*argsAst.nodes[0]);
    auto h = jit.emit_call(
        jit.module_->getFunction(rt::hash_any),
        {jit.extract_tag(arg), jit.extract_data(arg), line, col});
    jit.emit_value_release(arg);
    return jit.make_long(h);
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
    // JSON / Proc parse kwargs themselves on the fast path (handlers
    // below). Every other namespace returns nullptr so the call falls
    // through to the generic dispatch (compile_call → namespace_get →
    // property_get → compile_function_call_runtime_kwargs →
    // culebra_runtime_call_with_kwargs → the ns-method kwarg hook). The
    // hook resolves kwargs/defaults for methods carrying NsParamMeta and
    // raises a clean error for those that don't — matching the interp.
    static const std::set<std::string_view> kwarg_aware_ns = {"JSON", "Proc"};
    if (!kwarg_aware_ns.contains(ns)) {
      return nullptr;
    }
  }
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto line = builder_.getInt64(callAst.line);
  auto col = builder_.getInt64(callAst.column);

  // Proc.run / Proc.all route their (positional + kwargs) ARG_LIST through one
  // marshaller into the matching `_kw` runtime fn. Proc.race is positional-only
  // and falls through to the namespace-object trampoline.
  if (ns == "Proc" && method == "run") {
    return compile_single_positional_kwargs(jit, argsAst, callAst, "Proc.run",
                                            rt::proc_run_kw);
  }
  if (ns == "Proc" && method == "all") {
    return compile_single_positional_kwargs(jit, argsAst, callAst, "Proc.all",
                                            rt::proc_all_kw);
  }
  if (ns == "Proc" && method == "spawn") {
    return compile_single_positional_kwargs(jit, argsAst, callAst, "Proc.spawn",
                                            rt::proc_spawn_kw);
  }

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
  }

  if (ns == "FS") {
    // Whole-file read/write convenience (open+read/write+close).
    if (method == "read" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "FS.read argument");
      auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto s = emit_call(module_->getFunction(rt::read_file),
                         {ptr, line, col});
      emit_value_release(arg);
      return make_string(s);
    }
    if (method == "write" && argsAst.nodes.size() == 2) {
      auto p = compile(*argsAst.nodes[0]);
      emit_type_check(p, "String", "FS.write path");
      auto c = compile(*argsAst.nodes[1]);
      emit_type_check(c, "String", "FS.write content");
      auto pp = builder_.CreateIntToPtr(extract_data(p), ptrTy);
      auto cp = builder_.CreateIntToPtr(extract_data(c), ptrTy);
      emit_call(module_->getFunction(rt::write_file), {pp, cp, line, col});
      emit_value_release(p);
      emit_value_release(c);
      return make_nil();
    }

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

  if (ns == "_Time") {
    // Positional-only Long-nanos primitives. All `utc` args are
    // treated as i64 (Bool data extracts as 0/1).
    auto& a = argsAst.nodes;
    auto i64 = builder_.getInt64Ty();
    auto eat_bool_i64 = [&](const peg::Ast& ast) -> llvm::Value* {
      auto v = compile(ast);
      auto b = builder_.CreateZExt(extract_data(v), i64);
      emit_value_release(v);
      return b;
    };

    if (method == "now_nanos" && a.empty()) {
      auto n = emit_call(module_->getFunction(rt::time_now_nanos), {});
      return make_long(n);
    }
    if (method == "monotonic" && a.empty()) {
      auto d = emit_call(module_->getFunction(rt::time_monotonic), {});
      return make_float(d);
    }
    if (method == "sleep" && a.size() == 1) {
      auto secs = compile(*a[0]);
      auto d = jit.coerce_to_double(secs);
      emit_call(module_->getFunction(rt::time_sleep), {d});
      emit_value_release(secs);
      return make_nil();
    }
    if (method == "from_iso_nanos" && a.size() == 1) {
      auto arg = compile(*a[0]);
      emit_type_check(arg, "String", "_Time.from_iso_nanos argument");
      auto p = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto n = emit_call(module_->getFunction(rt::time_from_iso_nanos),
                         {p, line, col});
      emit_value_release(arg);
      return make_long(n);
    }
    if (method == "parse_nanos" && a.size() == 2) {
      auto s = compile(*a[0]);
      emit_type_check(s, "String", "_Time.parse_nanos argument");
      auto f = compile(*a[1]);
      emit_type_check(f, "String", "_Time.parse_nanos format");
      auto sp = builder_.CreateIntToPtr(extract_data(s), ptrTy);
      auto fp = builder_.CreateIntToPtr(extract_data(f), ptrTy);
      auto n = emit_call(module_->getFunction(rt::time_parse_nanos),
                         {sp, fp, line, col});
      emit_value_release(s);
      emit_value_release(f);
      return make_long(n);
    }
    if (method == "iso_nanos" && a.size() == 2) {
      auto n = value_to_long(compile(*a[0]));
      auto u = eat_bool_i64(*a[1]);
      auto s = emit_call(module_->getFunction(rt::time_iso_nanos), {n, u});
      return make_string(s);
    }
    if (method == "weekday_nanos" && a.size() == 2) {
      auto n = value_to_long(compile(*a[0]));
      auto u = eat_bool_i64(*a[1]);
      auto w = emit_call(module_->getFunction(rt::time_weekday_nanos), {n, u});
      return make_long(w);
    }
    if (method == "parts_nanos" && a.size() == 2) {
      auto n = value_to_long(compile(*a[0]));
      auto u = eat_bool_i64(*a[1]);
      auto o = emit_call(module_->getFunction(rt::time_parts_nanos), {n, u});
      return make_object(o);
    }
    if (method == "format_nanos" && a.size() == 3) {
      auto n = value_to_long(compile(*a[0]));
      auto f = compile(*a[1]);
      emit_type_check(f, "String", "_Time.format_nanos format");
      auto fp = builder_.CreateIntToPtr(extract_data(f), ptrTy);
      auto u = eat_bool_i64(*a[2]);
      auto s = emit_call(module_->getFunction(rt::time_format_nanos),
                         {n, fp, u});
      emit_value_release(f);
      return make_string(s);
    }
    if (method == "from_parts_nanos" && a.size() == 2) {
      auto obj = compile(*a[0]);
      emit_type_check(obj, "Object", "_Time.from_parts_nanos argument");
      auto op = builder_.CreateIntToPtr(extract_data(obj), ptrTy);
      auto u = eat_bool_i64(*a[1]);
      auto n = emit_call(module_->getFunction(rt::time_from_parts_nanos),
                         {op, u});
      emit_value_release(obj);
      return make_long(n);
    }
    if (method == "add_nanos" && a.size() == 8) {
      auto n  = value_to_long(compile(*a[0]));
      auto y  = value_to_long(compile(*a[1]));
      auto mo = value_to_long(compile(*a[2]));
      auto d  = value_to_long(compile(*a[3]));
      auto h  = value_to_long(compile(*a[4]));
      auto mi = value_to_long(compile(*a[5]));
      auto se = value_to_long(compile(*a[6]));
      auto u  = eat_bool_i64(*a[7]);
      auto r = emit_call(module_->getFunction(rt::time_add_nanos),
                         {n, y, mo, d, h, mi, se, u});
      return make_long(r);
    }
    if (method == "start_of_nanos" && a.size() == 3) {
      auto n = value_to_long(compile(*a[0]));
      auto unit = compile(*a[1]);
      emit_type_check(unit, "String", "_Time.start_of_nanos unit");
      auto up = builder_.CreateIntToPtr(extract_data(unit), ptrTy);
      auto u = eat_bool_i64(*a[2]);
      auto r = emit_call(module_->getFunction(rt::time_start_of_nanos),
                         {n, up, u, line, col});
      emit_value_release(unit);
      return make_long(r);
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
    // Activations relu/sigmoid/softmax are Tensor instance methods
    // (`t.relu()`), handled in JIT::compile_builtin_method.
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

inline llvm::Value* JitExtension::compile_single_positional_kwargs(
    JIT& jit, const peg::Ast& argsAst, const peg::Ast& callAst,
    const char* ctx, const char* rt_name) {
  CULEBRA_JIT_EXT_BODY_ALIASES(jit);
  using namespace peg::udl;
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto i64Ty = builder_.getInt64Ty();

  // Scan ARG_LIST: exactly one positional, the rest kwargs / splats.
  std::vector<const peg::Ast*> positional;
  std::vector<std::pair<std::string_view, const peg::Ast*>> explicit_kwargs;
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
        std::format("{}: expected exactly one positional argument", ctx),
        argsAst.line, argsAst.column);
  }

  auto cmd_val = compile(*positional[0]);
  std::vector<llvm::Constant*> kwKeys;
  std::vector<llvm::Value*> kwVals;
  for (auto& [name, ast] : explicit_kwargs) {
    kwKeys.push_back(builder_.CreateGlobalString(std::string(name), ".kwkey"));
    kwVals.push_back(compile(*ast));
  }
  std::vector<llvm::Value*> splatVals;
  for (auto* ast : splats) splatVals.push_back(compile(*ast));

  auto fn = builder_.GetInsertBlock()->getParent();
  llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
  auto alloc_slab = [&](llvm::Type* ty, size_t n,
                        const char* name) -> llvm::Value* {
    if (n == 0) return llvm::ConstantPointerNull::get(ptrTy);
    return entryB.CreateAlloca(ty, builder_.getInt64(static_cast<int64_t>(n)),
                              name);
  };
  auto store_at = [&](llvm::Value* base, llvm::Type* ty, size_t i,
                      llvm::Value* val) {
    auto slot = builder_.CreateInBoundsGEP(
        ty, base, {builder_.getInt64(static_cast<int64_t>(i))});
    builder_.CreateStore(val, slot);
  };
  auto keysSlab = alloc_slab(ptrTy, kwKeys.size(), "proc.kw.keys");
  auto valsSlab = alloc_slab(jit.valueType_, kwVals.size(), "proc.kw.vals");
  auto splatSlab = alloc_slab(jit.valueType_, splatVals.size(),
                              "proc.kw.splat");
  for (size_t i = 0; i < kwKeys.size(); i++) {
    store_at(keysSlab, ptrTy, i, kwKeys[i]);
    store_at(valsSlab, jit.valueType_, i, kwVals[i]);
  }
  for (size_t i = 0; i < splatVals.size(); i++) {
    store_at(splatSlab, jit.valueType_, i, splatVals[i]);
  }

  auto lineV = builder_.getInt64(callAst.line);
  auto colV = builder_.getInt64(callAst.column);
  auto r = emit_call(
      module_->getOrInsertFunction(
          rt_name, jit.valueType_, builder_.getInt8Ty(), i64Ty,
          i64Ty, ptrTy, ptrTy, i64Ty, ptrTy, i64Ty, i64Ty),
      {extract_tag(cmd_val), extract_data(cmd_val),
       builder_.getInt64(static_cast<int64_t>(kwVals.size())),
       keysSlab, valsSlab,
       builder_.getInt64(static_cast<int64_t>(splatVals.size())),
       splatSlab, lineV, colV});
  // The _kw fn doesn't consume the positional; release it here.
  emit_value_release(cmd_val);
  return r;
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
      "puts",    "print",
      "to_long", "to_float",  "to_string", "type_of", "hash",
      "Math",    "IO",        "FS",        "File",     "_Time",
      "Random",  "Sys",       "JSON",      "Tensor",   "GC",
      "_Regex",  "Proc",      "Isolate",   "Channel",  "Parallel"};
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
