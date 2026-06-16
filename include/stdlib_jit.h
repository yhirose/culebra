#pragma once

// JIT-side implementation of the Culebra standard library.
//
// Independent header. Include from main.cc (or any embedder) after
// jit.h. Provides the runtime functions called from JIT'd code and the
// `JitExtension` struct that fills `JIT::ExtensionHooks`. Embedders
// install the stdlib by calling `culebra::install_jit_stdlib()` once
// before `JIT::run()`.

#include <compress.h>
#include <hash.h>
#include <jit.h>
#include <proc.h>
#if defined(CULEBRA_HTTP_ENABLED)
#include <http.h>
#endif
#include <shared.h>
#include <regexlib.h>
#include <sendable_jit.h>  // JIT isolate transfer (jit_serialize, spawn, handle)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#include <unistd.h>  // isatty (IO.*_is_terminal)

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
  // Interruptible; empty string at EOF (unchanged behaviour).
  culebra::read_stdin_line_interruptible(line);
  return _culebra_heap_str(line);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_read_file(
    const char* path, int64_t line, int64_t col) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    throw culebra::CulebraError("IOError",
        std::format("FS.read: cannot open '{}'", path), line, col);
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
        std::format("FS.write: cannot open '{}'", path), line, col);
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
CUL_MATH_F2F(sin,  std::sin(x))
CUL_MATH_F2F(cos,  std::cos(x))
CUL_MATH_F2F(tan,  std::tan(x))
CUL_MATH_F2F(asin, std::asin(x))
CUL_MATH_F2F(acos, std::acos(x))
CUL_MATH_F2F(atan, std::atan(x))
#undef CUL_MATH_F2F

// atan2(y, x): two numeric args -> Float (radians). Mirrors the F2F family
// but binary; either Long or Float coerces to double.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_atan2(
    int8_t yt, int64_t yd, int8_t xt, int64_t xd, int64_t line, int64_t col) {
  if ((yt != TAG_LONG && yt != TAG_FLOAT) ||
      (xt != TAG_LONG && xt != TAG_FLOAT))
    throw_type_error_at(line, col);
  double y = _culebra_coerce_num(yt, yd);
  double x = _culebra_coerce_num(xt, xd);
  return {TAG_FLOAT, _culebra_double_to_bits(std::atan2(y, x))};
}

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
  // min/max are variadic over >=1 numeric arg (max(5) == 5), matching the
  // interpreter's numeric_reduce (which rejects only the empty arg list).
  if (n < 1) throw_type_error_at(line, col);
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
      int8_t lt, int64_t ld, int8_t rt, int64_t rd,                     \
      int64_t line, int64_t col) {                                      \
    /* Reject non-numeric operands with a positioned error (the inline  \
       fast path passes the call site; the as-value adapter passes 0,0  \
       and the dispatch backfills) — matching the interp's positioned   \
       `type error`, which was positionless under the JIT before. */     \
    if ((lt != TAG_LONG && lt != TAG_FLOAT) ||                          \
        (rt != TAG_LONG && rt != TAG_FLOAT))                            \
      throw_type_error_at(line, col);                                   \
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
      std::format("{}: {}.", msg, ec.message()), line, col);
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
// in stdlib_interp.h) — interp registers it lazily, JIT/AOT splice it
// selectively into the entry module via `splice_stdlib_preamble`.
// Timestamps are
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
        std::format("_Time.from_iso_nanos: invalid ISO 8601 '{}'",
                    s ? s : ""),
        line, col);
  }
  return *r;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_time_parse_nanos(
    const char* s, const char* fmt, int64_t line, int64_t col) {
  std::tm tm{};
  if (!strptime(s ? s : "", fmt ? fmt : "", &tm)) {
    throw culebra::CulebraError("ValueError",
        std::format("_Time.parse_nanos: '{}' does not match '{}'",
                    s ? s : "", fmt ? fmt : ""),
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
                    "(year/month/day/hour/minute)",
                    std::string(u)),
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
      // Canonical typed-param message, matching the interp binder
      // (`parameter '<name>' expects <Type>`). `ctx_` is kept for the
      // ad-hoc Proc messages that still build their own strings.
      fail(std::format("type error: parameter '{}' expects {}", name,
                       want_name));
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
      fail(std::format("type error: parameter '{}' expects String", name));
    }
    std::string s(_culebra_str_view(v.tag, v.data));
    _culebra_value_release_impl(v.tag, v.data);
    return s;
  }

  // Like take_string but for an optional (`String?`) kwarg: an explicit nil is
  // treated as absent (→ nullopt), matching interp's nil-default params (cwd).
  // A non-String / non-Nil value fails with the `String?` message.
  std::optional<std::string> take_string_or_nil(std::string_view name) {
    auto it = merged_.find(name);
    if (it == merged_.end()) return std::nullopt;
    auto v = it->second;
    merged_.erase(it);
    if (v.tag == TAG_NIL) return std::nullopt;
    if (v.tag != TAG_STRING && v.tag != TAG_STRINGVIEW) {
      _culebra_value_release_impl(v.tag, v.data);
      fail(std::format("type error: parameter '{}' expects String?", name));
    }
    std::string s(_culebra_str_view(v.tag, v.data));
    _culebra_value_release_impl(v.tag, v.data);
    return s;
  }

  // Resolve an `env` (`Object?`) kwarg into name/value pairs (appended to
  // `out`). Returns true if a non-nil Object was present. An explicit nil is
  // absent (interp's nil default); a non-Object / non-Nil fails `Object?`; a
  // non-String value inside is tagged with ctx_.
  bool take_env(std::vector<std::pair<std::string, std::string>>& out) {
    auto it = merged_.find("env");
    if (it == merged_.end()) return false;
    auto raw = it->second;
    merged_.erase(it);
    if (raw.tag == TAG_NIL) return false;
    if (raw.tag != TAG_OBJECT) {
      _culebra_value_release_impl(raw.tag, raw.data);
      fail("type error: parameter 'env' expects Object?");
    }
    bool ok = _ns_env_object_pairs(reinterpret_cast<JitObject*>(raw.data), out);
    _culebra_value_release_impl(raw.tag, raw.data);
    if (!ok) fail(std::string(ctx_) + ": env values must be String", "TypeError");
    return true;
  }

  // Resolve a `share` Object (name -> SharedBuffer handle) into CULEBRA_SHARE_*
  // env entries (appended to `env_out`) plus the fds the child inherits
  // (appended to `fds_out`). Mirrors the interp proc_parse_launch share branch.
  // Returns true if `share` was present.
  bool take_share(std::vector<std::pair<std::string, std::string>>& env_out,
                  std::vector<int>& fds_out) {
    // `share` is an optional Object (nil = absent, like interp's nil default);
    // a non-Object / non-Nil reuses interp's descriptive top-level message.
    auto it = merged_.find("share");
    if (it == merged_.end()) return false;
    auto raw = it->second;
    merged_.erase(it);
    if (raw.tag == TAG_NIL) return false;
    if (raw.tag != TAG_OBJECT) {
      _culebra_value_release_impl(raw.tag, raw.data);
      fail(std::string(ctx_) +
           ": share must be an Object of name -> SharedBuffer");
    }
    // Collect (name, buffer id) first so a validation throw can't leak `raw`.
    std::vector<std::pair<std::string, long>> entries;
    auto* obj = reinterpret_cast<JitObject*>(raw.data);
    if (obj->shape) {
      for (size_t k = 0; k < obj->shape->names.size(); k++) {
        const std::string& name = obj->shape->names[k];
        const JitValue& bv = obj->slots[k].value;
        size_t si = (bv.tag == TAG_OBJECT)
                        ? reinterpret_cast<JitObject*>(bv.data)
                              ->find_slot("__sharedbuffer_id__")
                        : static_cast<size_t>(-1);
        if (si == static_cast<size_t>(-1)) {
          _culebra_value_release_impl(raw.tag, raw.data);
          fail(std::format("{}: share `{}` must be a SharedBuffer", ctx_, name));
        }
        entries.emplace_back(
            name, reinterpret_cast<JitObject*>(bv.data)->slots[si].value.data);
      }
    }
    _culebra_value_release_impl(raw.tag, raw.data);
    for (auto& [name, id] : entries) {
      auto [fd, env_val] = culebra::prepare_share_buffer(id, name);
      env_out.emplace_back(culebra::share_env_key(name), std::move(env_val));
      fds_out.push_back(fd);
    }
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
    throw culebra::CulebraError(std::string(kind), std::format("{}", msg),
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
        std::format("JSON.parse: {}", msg), line, col);
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
          "JSON.parse: trailing characters", jp.line, jp.col);
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
            "JSON.parse(lines: true): trailing characters", jp.line, jp.col);
      }
      culebra_runtime_array_push(arr, v.tag, v.data);
    }
    if (j == n) break;
    i = j + 1;
    lineno++;
  }
  return {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
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
        std::format("{}: commands must be Array", ctx), line, col);
  }
  auto* outer = reinterpret_cast<JitArray*>(commands_data);
  std::vector<std::vector<std::string>> commands;
  commands.reserve(outer->size);
  for (size_t i = 0; i < outer->size; i++) {
    const JitValue& cv = outer->items[i];
    if (cv.tag != TAG_ARRAY) {
      throw culebra::CulebraError("TypeError",
          std::format("{}: each command must be an Array of String", ctx),
          line, col);
    }
    auto* inner = reinterpret_cast<JitArray*>(cv.data);
    if (inner->size == 0) {
      throw culebra::CulebraError("ValueError",
          std::format("{}: empty command", ctx), line, col);
    }
    std::vector<std::string> argv;
    argv.reserve(inner->size);
    for (size_t j = 0; j < inner->size; j++) {
      const JitValue& e = inner->items[j];
      if (e.tag != TAG_STRING && e.tag != TAG_STRINGVIEW) {
        throw culebra::CulebraError("TypeError",
            std::format("{}: command elements must be String", ctx),
            line, col);
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
    int64_t line, int64_t col,
    const std::vector<int>* inherit_fds = nullptr) {
  if (cmd_tag != TAG_ARRAY) {
    throw culebra::CulebraError("TypeError",
        "Proc.run: cmd must be Array", line, col);
  }
  auto* cmd = reinterpret_cast<JitArray*>(cmd_data);
  std::vector<std::string> argv;
  argv.reserve(cmd->size);
  for (size_t i = 0; i < cmd->size; i++) {
    const JitValue& e = cmd->items[i];
    if (e.tag != TAG_STRING && e.tag != TAG_STRINGVIEW) {
      throw culebra::CulebraError("TypeError",
          "Proc.run: command elements must be String", line, col);
    }
    argv.emplace_back(_culebra_str_view(e.tag, e.data));
  }
  if (argv.empty()) {
    throw culebra::CulebraError("ValueError",
        "Proc.run: empty command", line, col);
  }

  auto oc = culebra::proc::run_command(argv, cwd, env_over, stdin_data,
                                       timeout > 0 ? timeout : 0, inherit_fds);
  if (!oc.spawned) {
    throw culebra::CulebraError("ProcessError",
        std::format("Proc.run: {} failed: {}.", oc.err_what,
                    std::system_category().message(oc.err_no)),
        line, col);
  }
  if (check && !oc.result.ok) {
    throw culebra::CulebraError("ProcessError",
        std::format("Proc.run: command {}",
                    culebra::proc::failure_detail(oc.result)), line, col);
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
  // Resolve in interp's param-declaration order (cwd, env, stdin, check,
  // timeout) so that when several kwargs are ill-typed the *first* reported
  // error matches the interp binder.
  std::string cwd_str;
  const std::string* cwd_ptr = nullptr;
  if (auto v = kw.take_string_or_nil("cwd")) {
    cwd_str = std::move(*v);
    cwd_ptr = &cwd_str;
  }
  std::vector<std::pair<std::string, std::string>> overrides;
  bool has_env = kw.take_env(overrides);
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
  std::vector<int> share_fds;
  bool has_share = kw.take_share(overrides, share_fds);
  const std::vector<std::pair<std::string, std::string>>* env_ptr =
      (has_env || has_share) ? &overrides : nullptr;
  const std::vector<int>* fds_ptr = share_fds.empty() ? nullptr : &share_fds;
  kw.validate_consumed();
  return _culebra_proc_run_impl(cmd_tag, cmd_data, cwd_ptr, env_ptr,
                                stdin_data, check, timeout, line, col, fds_ptr);
}

// Proc.all core (shared by trampoline + kwarg adapter). `commands` is not
// consumed. Returns an Array of result Objects (allSettled, input order).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue _culebra_proc_all_impl(
    int8_t commands_tag, int64_t commands_data, int64_t limit, int64_t timeout,
    bool fail_fast, int64_t retries, int64_t line, int64_t col,
    const std::vector<std::pair<std::string, std::string>>* env = nullptr,
    const std::vector<int>* inherit_fds = nullptr) {
  auto commands = _culebra_proc_parse_commands(commands_tag, commands_data,
                                               "Proc.all", line, col);
  if (limit < 0) limit = 0;
  size_t failed = SIZE_MAX;
  auto outcomes = culebra::proc::run_all(
      commands, static_cast<size_t>(limit), nullptr, env, nullptr,
      timeout > 0 ? timeout : 0, fail_fast, &failed, retries > 0 ? retries : 0,
      inherit_fds);
  if (fail_fast && failed != SIZE_MAX) {
    throw culebra::CulebraError("ProcessError",
        std::format("Proc.all: command {} {}", failed,
                    culebra::proc::outcome_detail(outcomes[failed])),
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
  std::vector<std::pair<std::string, std::string>> overrides;
  std::vector<int> share_fds;
  bool has_share = kw.take_share(overrides, share_fds);
  kw.validate_consumed();
  return _culebra_proc_all_impl(
      commands_tag, commands_data, limit, timeout, fail_fast, retries, line,
      col, has_share ? &overrides : nullptr,
      share_fds.empty() ? nullptr : &share_fds);
}

// Proc.race(commands) — first to finish wins, the rest are killed. `commands`
// is not consumed. Empty list throws ValueError.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue _culebra_proc_race_impl(
    int8_t commands_tag, int64_t commands_data, int64_t line, int64_t col,
    const std::vector<std::pair<std::string, std::string>>* env = nullptr,
    const std::vector<int>* inherit_fds = nullptr) {
  auto commands = _culebra_proc_parse_commands(commands_tag, commands_data,
                                               "Proc.race", line, col);
  if (commands.empty()) {
    throw culebra::CulebraError("ValueError",
        "Proc.race: empty command list", line, col);
  }
  auto [winner, oc] =
      culebra::proc::run_race(commands, 0, nullptr, env, nullptr, inherit_fds);
  (void)winner;
  return {TAG_OBJECT,
          reinterpret_cast<int64_t>(_culebra_proc_outcome_to_object(oc, line, col))};
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_proc_race(
    int8_t commands_tag, int64_t commands_data, int64_t line, int64_t col) {
  return _culebra_proc_race_impl(commands_tag, commands_data, line, col);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_proc_race_kw(
    int8_t commands_tag, int64_t commands_data,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs, int64_t line, int64_t col) {
  _JitKwargResolver kw(n_kw, kw_keys, kw_vals, n_splat, splat_objs, line, col,
                       "Proc.race");
  std::vector<std::pair<std::string, std::string>> overrides;
  std::vector<int> share_fds;
  bool has_share = kw.take_share(overrides, share_fds);
  kw.validate_consumed();
  return _culebra_proc_race_impl(commands_tag, commands_data, line, col,
                                 has_share ? &overrides : nullptr,
                                 share_fds.empty() ? nullptr : &share_fds);
}

#if defined(CULEBRA_HTTP_ENABLED)
// Defined in jit.h (forward-declared there too); HTTP's `json` method
// needs it before this point.
CULEBRA_RT_INLINE JitClosure* _jit_make_handle_method(
    JitValue (*fn)(JitClosure*, JitValue, int64_t, JitValue*), size_t arity,
    const JitParamMeta* meta);

// `r.json()` — parse the response body as JSON. Method ABI: self arrives +1
// (release before returning), mirroring the proc/file handle methods.
CULEBRA_RT_INLINE JitValue _jit_http_json(JitClosure*, JitValue self, int64_t,
                                          JitValue*) {
  auto* o = reinterpret_cast<JitObject*>(self.data);
  size_t i = o->find_slot("body");
  std::string body;
  if (i != static_cast<size_t>(-1)) {
    JitValue b = o->slots[i].value;
    if (b.tag == TAG_STRING || b.tag == TAG_STRINGVIEW) {
      body = _culebra_str_view(b.tag, b.data);
    }
  }
  JitValue r = culebra_runtime_json_parse(body.c_str(), "auto", 0);
  culebra_runtime_value_release(self.tag, self.data);
  return r;
}

// Build the `{status, ok, body, headers}` response Object from an HttpResult,
// plus a `json()` method. `ok` is 2xx; a 4xx/5xx is a completed round-trip
// (ok:false). Transport failures never reach here — _culebra_http_run throws
// HttpError first.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* _culebra_http_result_to_object(
    culebra::http::HttpResult& r, int64_t line, int64_t col) {
  auto* o = culebra_runtime_object_new();
  culebra_runtime_object_set(o, "status", false, TAG_LONG, r.status, line, col);
  culebra_runtime_object_set(o, "ok", false, TAG_BOOL,
      (r.status >= 200 && r.status < 300) ? 1 : 0, line, col);
  culebra_runtime_object_set(o, "reason", false, TAG_STRING,
      reinterpret_cast<int64_t>(_culebra_heap_str(r.reason)), line, col);
  culebra_runtime_object_set(o, "body", false, TAG_STRING,
      reinterpret_cast<int64_t>(_culebra_heap_str(r.body)), line, col);
  auto* headers = culebra_runtime_object_new();
  for (auto& [k, v] : r.headers) {
    culebra_runtime_object_set(headers, k.c_str(), false, TAG_STRING,
        reinterpret_cast<int64_t>(_culebra_heap_str(v)), line, col);
  }
  culebra_runtime_object_set(o, "headers", false, TAG_OBJECT,
      reinterpret_cast<int64_t>(headers), line, col);
  culebra_runtime_object_set(o, "json", false, TAG_FUNC,
      reinterpret_cast<int64_t>(_jit_make_handle_method(_jit_http_json, 0)),
      line, col);
  return o;
}

// Backing state for an `into:` response sink, owned by the adapter for the
// whole request: the output file (String into) and any callback exception
// (rethrown after the request returns).
struct JitHttpInto {
  std::ofstream ofs;
  std::exception_ptr eptr;
};

// Configure `req.body_sink` from the `into` slab value: nil → buffer the body;
// TAG_STRING → write to that file path; TAG_FUNC → call per chunk. Else →
// TypeError. Mirrors interp http_setup_into.
inline void _http_setup_into(JitValue into, culebra::http::HttpRequest& req,
                             JitHttpInto& st, const char* ctx) {
  if (into.tag == TAG_NIL) return;
  if (into.tag == TAG_STRING || into.tag == TAG_STRINGVIEW) {
    std::string path(_culebra_str_view(into.tag, into.data));
    st.ofs.open(path, std::ios::binary);
    if (!st.ofs) {
      throw culebra::CulebraError(
          "IOError",
          std::format("{}: cannot open '{}' for writing", ctx, path), 0, 0);
    }
    req.body_sink = [&st](const char* d, size_t n) {
      st.ofs.write(d, static_cast<std::streamsize>(n));
      return static_cast<bool>(st.ofs);
    };
  } else if (into.tag == TAG_FUNC) {
    auto* cb = reinterpret_cast<JitClosure*>(into.data);
    req.body_sink = [cb, &st](const char* d, size_t n) -> bool {
      try {
        JitValue chunk{TAG_STRING, reinterpret_cast<int64_t>(
                                       _culebra_heap_str(std::string(d, n)))};
        JitValue r = _culebra_invoke1(cb, chunk);  // consumes chunk's +1
        _culebra_value_release_impl(r.tag, r.data);
        return true;
      } catch (...) {
        st.eptr = std::current_exception();  // abort; rethrown after send
        return false;
      }
    };
  } else {
    throw culebra::CulebraError(
        "TypeError",
        std::format("{}: into must be a String path or a Function", ctx), 0, 0);
  }
}

// Run a request (whose sink may have been set by _http_setup_into): rethrow a
// callback exception first, then map a transport failure to HttpError, else
// return the response Object. Shared by every Http.* adapter.
inline JitValue _http_run_into(culebra::http::HttpRequest& req,
                               JitHttpInto& st, const char* ctx) {
  auto r = culebra::http::http_request(req);
  if (st.eptr) std::rethrow_exception(st.eptr);
  if (!r.ok) {
    throw culebra::CulebraError("HttpError", std::format("{}: {}", ctx, r.error),
                                0, 0);
  }
  return {TAG_OBJECT,
          reinterpret_cast<int64_t>(_culebra_http_result_to_object(r, 0, 0))};
}

// Configure the request body from the `body` slab value: TAG_STRING → whole;
// TAG_FUNC → producer (called per chunk, returns next chunk String or nil),
// streamed chunked. Else → TypeError. Mirrors interp http_setup_body.
inline void _http_setup_body(JitValue bodyv, JitValue jsonv, JitValue formv,
                             JitValue ct, culebra::http::HttpRequest& req,
                             JitHttpInto& st, const char* ctx) {
  // At most one of body / json / form. `json` → application/json; `form` →
  // application/x-www-form-urlencoded.
  bool has_json = jsonv.tag != TAG_NIL;
  bool has_form = formv.tag != TAG_NIL;
  bool has_body =
      bodyv.tag == TAG_FUNC ||
      ((bodyv.tag == TAG_STRING || bodyv.tag == TAG_STRINGVIEW) &&
       !std::string_view(_culebra_str_view(bodyv.tag, bodyv.data)).empty());
  if (has_json + has_form + has_body > 1) {
    throw culebra::CulebraError(
        "TypeError",
        std::format("{}: pass at most one of body, json, form", ctx), 0, 0);
  }
  if (has_json) {
    req.body = culebra_runtime_json_stringify(jsonv.tag, jsonv.data, 0, 0, 0);
    req.content_type = "application/json";
    return;
  }
  if (has_form) {
    if (formv.tag != TAG_OBJECT) {
      throw culebra::CulebraError("TypeError",
          std::format("{}: form must be an Object of String", ctx), 0, 0);
    }
    culebra::http::HeaderList pairs;
    if (!_ns_env_object_pairs(reinterpret_cast<JitObject*>(formv.data), pairs)) {
      throw culebra::CulebraError("TypeError",
          std::format("{}: form values must be String", ctx), 0, 0);
    }
    req.body = culebra::http::encode_query(pairs);
    req.content_type = "application/x-www-form-urlencoded";
    return;
  }
  // ct / body may be nil when this method is reached positional-only (the slow
  // path doesn't materialize defaults) — treat nil as the default: ct =
  // "text/plain", body = empty (no body), matching interp's resolved defaults.
  // interp resolves content_type via the strict Value::to_string(), so a
  // present non-String raises `expected String, got <T>` rather than coercing.
  if (ct.tag == TAG_STRING || ct.tag == TAG_STRINGVIEW) {
    req.content_type = std::string(_culebra_str_view(ct.tag, ct.data));
  } else if (ct.tag == TAG_NIL) {
    req.content_type = "text/plain";
  } else {
    culebra::throw_type_mismatch("String", _culebra_tag_name(ct.tag), 0, 0);
  }
  if (bodyv.tag == TAG_NIL) return;  // missing → no request body
  if (bodyv.tag == TAG_FUNC) {
    auto* producer = reinterpret_cast<JitClosure*>(bodyv.data);
    req.body_source = [producer, &st, ctx](std::string& out) -> bool {
      try {
        JitValue r = _culebra_invoke0(producer);
        if (r.tag == TAG_NIL) return false;  // end of stream
        if (r.tag != TAG_STRING && r.tag != TAG_STRINGVIEW) {
          _culebra_value_release_impl(r.tag, r.data);
          throw culebra::CulebraError(
              "TypeError",
              std::format("{}: body producer must return a String or nil", ctx),
              0, 0);
        }
        out = _culebra_str_view(r.tag, r.data);
        _culebra_value_release_impl(r.tag, r.data);
        return true;
      } catch (...) {
        st.eptr = std::current_exception();  // abort; rethrown after send
        return false;
      }
    };
  } else if (bodyv.tag == TAG_STRING || bodyv.tag == TAG_STRINGVIEW) {
    req.body = _culebra_str_view(bodyv.tag, bodyv.data);
  } else {
    throw culebra::CulebraError(
        "TypeError",
        std::format("{}: body must be a String or a Function (producer)", ctx),
        0, 0);
  }
}
#endif  // CULEBRA_HTTP_ENABLED

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

CULEBRA_RT_INLINE JitValue _culebra_proc_build_handle(long pid, int out_fd,
                                                      int err_fd) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_pid", JitValue{TAG_LONG, pid}, true);
  h->set_or_append("_out", JitValue{TAG_LONG, out_fd}, true);
  h->set_or_append("_err", JitValue{TAG_LONG, err_fd}, true);
  h->set_or_append("_done", JitValue{TAG_BOOL, 0}, true);
  h->set_or_append("_result", JitValue{TAG_NIL, 0}, true);
  // A native handle is not Sendable — reject it at the serialize boundary
  // (jit_serialize checks __nonsendable__), mirroring the interp handle.
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  _jit_handle_bind_method(h, "wait", _jit_handle_wait, 0);
  _jit_handle_bind_method(h, "poll", _jit_handle_poll, 0);
  _jit_handle_bind_method(h, "kill", _jit_handle_kill, 0);
  _jit_handle_bind_method(h, "drop", _jit_handle_drop, 0);
  // Bind chokepoint (has_drop + owned-stack registration) — see the
  // File handle note below.
  _jit_owned_bind_drop(h);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// --- File handle (JIT) ---
// Methods read `_id` (Long) from the handle and operate on the shared
// side table (culebra::_file_* helpers, defined in stdlib_interp.h). Same
// method ABI as the Proc handle: self arrives +1 and is released here,
// except drop (called from the destructor's drop protocol, which manages
// refcount itself — see _jit_handle_drop).

// Binder-order argument checks, the same convention as wrap.h's
// jit_check_args (interp parity): a missing required argument is an
// ArityError at the call site, a wrong-typed one a TypeError at the
// argument's threaded position. A TAG_UNFILLED slot (defaulted kwarg the
// resolver left empty) counts as absent, never wrong-typed.
CULEBRA_RT_INLINE bool _jit_file_arg_present(int64_t n, JitValue* args,
                                             size_t i) {
  return static_cast<int64_t>(i) < n && args[i].tag != TAG_UNFILLED;
}
[[noreturn]] CULEBRA_RT_INLINE void _jit_file_missing_arg(JitValue self,
                                                          const char* pname) {
  culebra_runtime_value_release(self.tag, self.data);
  throw culebra::CulebraError(
      "ArityError", std::format("missing required argument '{}'", pname),
      _jit_call_site_line, _jit_call_site_col);
}
[[noreturn]] CULEBRA_RT_INLINE void _jit_file_param_type_error(
    JitValue self, const char* pname, const char* expected, size_t i) {
  culebra_runtime_value_release(self.tag, self.data);
  const bool ap = static_cast<int>(i) < _jit_argpos_n;
  throw culebra::CulebraError(
      "TypeError",
      std::format("type error: parameter '{}' expects {}", pname, expected),
      ap ? _jit_argpos_line[i]
         : (i == 0 ? _jit_call_arg0_line : _jit_call_site_line),
      ap ? _jit_argpos_col[i]
         : (i == 0 ? _jit_call_arg0_col : _jit_call_site_col));
}
// Untyped-param body check ("expected X, got Y" at the call site), the
// shape the interp's to_long()/to_string() conversions produce.
[[noreturn]] CULEBRA_RT_INLINE void _jit_file_body_type_error(
    JitValue self, const char* expected, int8_t got_tag) {
  culebra_runtime_value_release(self.tag, self.data);
  culebra_runtime_type_error_typed(_jit_call_site_line, _jit_call_site_col,
                                   expected, got_tag);
}

CULEBRA_RT_INLINE JitValue _jit_file_read(JitClosure*, JitValue self,
                                          int64_t n, JitValue* args) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int64_t id = _jit_handle_long(h, "_id");
  // `n` is untyped with default nil (nil → rest of file), so a non-Long
  // is the interp's to_long() body error, not a param error.
  const bool has_n = _jit_file_arg_present(n, args, 0) &&
                     args[0].tag != TAG_NIL;
  if (has_n && args[0].tag != TAG_LONG)
    _jit_file_body_type_error(self, "Long", args[0].tag);
  std::string out = has_n ? culebra::_file_read_n(id, args[0].data, 0, 0)
                          : culebra::_file_read_all(id, 0, 0);
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(out))};
}
CULEBRA_RT_INLINE JitValue _jit_file_write(JitClosure*, JitValue self,
                                           int64_t n, JitValue* args) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int64_t id = _jit_handle_long(h, "_id");
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "data");
  // A `String` param rejects a StringView slice on every path (wrap.h's
  // jit_check_args predicate; interp type_matches) — same here.
  if (args[0].tag != TAG_STRING)
    _jit_file_param_type_error(self, "data", "String", 0);
  auto sv = _culebra_str_view(args[0].tag, args[0].data);
  culebra::_file_write(id, sv, 0, 0);
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
  if (!_jit_file_arg_present(n, args, 0))
    _jit_file_missing_arg(self, "offset");
  if (args[0].tag != TAG_LONG)
    _jit_file_param_type_error(self, "offset", "Long", 0);
  long off = args[0].data;
  // `whence` is untyped with default "set" — a non-String is the interp's
  // to_string() body error.
  std::string_view whence = "set";
  if (_jit_file_arg_present(n, args, 1)) {
    if (args[1].tag != TAG_STRING && args[1].tag != TAG_STRINGVIEW)
      _jit_file_body_type_error(self, "String", args[1].tag);
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
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "n");
  if (args[0].tag != TAG_LONG)
    _jit_file_param_type_error(self, "n", "Long", 0);
  auto* it = _file_iter_build(self, /*chunks=*/true, args[0].data);
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(it)};
}

CULEBRA_RT_INLINE JitValue _culebra_file_build_handle(int64_t id) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_id", JitValue{TAG_LONG, id}, false);
  // A native handle is not Sendable — reject it at the serialize boundary
  // (jit_serialize checks __nonsendable__), mirroring the interp handle.
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  // Param metadata so the named-param methods bind keyword arguments by
  // name like the interp's File handle (`f.read(n: 4)`, `f.seek(offset:
  // 10)`). The hand-written thunks already tolerate an omitted defaulted
  // param (a TAG_UNFILLED arg falls to the thunk's own default), so a
  // partial kwargs call (`seek(offset: 10)`) works. Built once.
  static const JitParamMeta* read_meta =
      _jit_make_handle_meta({"n"}, {true});
  static const JitParamMeta* write_meta =
      _jit_make_handle_meta({"data"}, {false});
  static const JitParamMeta* seek_meta =
      _jit_make_handle_meta({"offset", "whence"}, {false, true});
  static const JitParamMeta* chunks_meta =
      _jit_make_handle_meta({"n"}, {false});
  _jit_handle_bind_method(h, "read", _jit_file_read, 0, read_meta);
  _jit_handle_bind_method(h, "write", _jit_file_write, 1, write_meta);
  _jit_handle_bind_method(h, "flush", _jit_file_flush, 0);
  _jit_handle_bind_method(h, "seek", _jit_file_seek, 1, seek_meta);
  _jit_handle_bind_method(h, "tell", _jit_file_tell, 0);
  _jit_handle_bind_method(h, "lines", _jit_file_lines, 0);
  _jit_handle_bind_method(h, "chunks", _jit_file_chunks, 1, chunks_meta);
  _jit_handle_bind_method(h, "close", _jit_file_close, 0);
  _jit_handle_bind_method(h, "drop", _jit_file_drop, 0);
  // Through the bind chokepoint, not a bare `has_drop = true`: the
  // handle must also register on the owned stack, or a cycle-held File
  // would miss its deterministic scope-exit drop (the interp's handle
  // registers via initialize()).
  _jit_owned_bind_drop(h);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// Spawn core: parse argv, spawn detached, return a live handle (or throw).
CULEBRA_RT_INLINE JitValue _culebra_proc_spawn_build(
    int8_t cmd_tag, int64_t cmd_data, const std::string* cwd,
    const std::vector<std::pair<std::string, std::string>>* env_over,
    const std::string& stdin_data, int64_t line, int64_t col,
    const std::vector<int>* inherit_fds = nullptr) {
  if (cmd_tag != TAG_ARRAY) {
    throw culebra::CulebraError("TypeError",
        "Proc.spawn: cmd must be Array", line, col);
  }
  auto* cmd = reinterpret_cast<JitArray*>(cmd_data);
  std::vector<std::string> argv;
  argv.reserve(cmd->size);
  for (size_t i = 0; i < cmd->size; i++) {
    const JitValue& e = cmd->items[i];
    if (e.tag != TAG_STRING && e.tag != TAG_STRINGVIEW) {
      throw culebra::CulebraError("TypeError",
          "Proc.spawn: command elements must be String", line, col);
    }
    argv.emplace_back(_culebra_str_view(e.tag, e.data));
  }
  if (argv.empty()) {
    throw culebra::CulebraError("ValueError",
        "Proc.spawn: empty command", line, col);
  }
  auto sr = culebra::proc::spawn_detached(argv, cwd, env_over, stdin_data,
                                          inherit_fds);
  if (!sr.spawned) {
    throw culebra::CulebraError("ProcessError",
        std::format("Proc.spawn: {} failed: {}.", sr.err_what,
                    std::system_category().message(sr.err_no)),
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
  // Param-declaration order: cwd, env, stdin, share (see Proc.run note).
  std::string cwd_str;
  const std::string* cwd_ptr = nullptr;
  if (auto v = kw.take_string_or_nil("cwd")) {
    cwd_str = std::move(*v);
    cwd_ptr = &cwd_str;
  }
  std::vector<std::pair<std::string, std::string>> overrides;
  bool has_env = kw.take_env(overrides);
  std::string stdin_data;
  if (auto v = kw.take_string("stdin")) stdin_data = std::move(*v);
  std::vector<int> share_fds;
  bool has_share = kw.take_share(overrides, share_fds);
  const std::vector<std::pair<std::string, std::string>>* env_ptr =
      (has_env || has_share) ? &overrides : nullptr;
  const std::vector<int>* fds_ptr = share_fds.empty() ? nullptr : &share_fds;
  kw.validate_consumed();
  return _culebra_proc_spawn_build(cmd_tag, cmd_data, cwd_ptr, env_ptr,
                                   stdin_data, line, col, fds_ptr);
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
struct NsMethod;     // defined below; referenced by compile_ns_method_kwargs
struct NsParamMeta;  // defined below; referenced by compile_single_positional_kwargs

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
  static llvm::Value* compile_nested_ns_call(JIT& jit,
                                              std::string_view ns,
                                              std::string_view sub,
                                              std::string_view method,
                                              const peg::Ast& argsAst,
                                              const peg::Ast& callAst);
  static llvm::Value* compile_ns_prop(JIT& jit,
                                        std::string_view ns,
                                        std::string_view prop);

  // Marshals a `(cmd, kwargs..., **splat)` ARG_LIST into a runtime `_kw`
  // function (cmd_tag, cmd_data, keys/vals slabs, splat slab, line, col).
  // Shared by Proc.run (rt::proc_run_kw) / Proc.all (rt::proc_all_kw) /
  // Proc.spawn. Extra positionals after cmd bind to the leading params by
  // name (from `meta`) — i.e. `Proc.run(cmd, "/tmp")` sets cwd positionally,
  // matching interp — so they're folded into the kwarg slab here.
  static llvm::Value* compile_single_positional_kwargs(
      JIT& jit, const peg::Ast& argsAst, const peg::Ast& callAst,
      const char* ctx, const char* rt_name, const NsParamMeta* meta);

  // Generic compile of an `Ns.method(pos..., kwargs...)` call against a
  // statically-resolved NsMethod (Http). Compiles each positional once,
  // emits an inline type check at the *argument* position for any leading
  // positional whose NsParam carries a declared type, then dispatches the
  // compiled args through `culebra_runtime_ns_method_call_kw` (which consumes
  // them — no caller-side release).
  static llvm::Value* compile_ns_method_kwargs(
      JIT& jit, const NsMethod* m, const peg::Ast& argsAst,
      const peg::Ast& callAst);

  // Evaluate the first `eval_count` args (source order, for side effects), then
  // emit a runtime IR throw. Used by the stdlib compile paths for the
  // statically-known malformed calls (arity / positional-vs-keyword conflict /
  // duplicate keyword / positional-after-keyword) so the error is catchable on
  // every backend, matching interp's evaluate-then-error semantics. Returns a
  // placeholder value (unreachable: emit_throw_error is noreturn).
  static llvm::Value* emit_malformed_arg_throw(
      JIT& jit, const peg::Ast& argsAst, size_t eval_count, const char* kind,
      const std::string& msg, long line, long col);

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
        .compile_nested_ns_call = &compile_nested_ns_call,
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

[[noreturn]] inline void arity_error(const char* /*ns*/, const char* /*method*/,
                                       int expected, int64_t got,
                                       int64_t line = 0, int64_t col = 0) {
  // Nameless count message (see ns_fn_arity_error_message): the interpreter
  // does not carry the qualified name on these FunctionValues, so dropping it
  // keeps `Math.abs(1, 2)` and `let f = Math.abs; f(1, 2)` byte-identical
  // across backends.
  culebra::throw_runtime_error_at(
      "ArityError", culebra::ns_fn_arity_error_message(expected, got), line,
      col);
}

inline const char* take_str(JitValue v) {
  return v.tag == TAG_STRING ? reinterpret_cast<const char*>(v.data) : "";
}
// Length-authoritative view of a TAG_STRING (header-backed, so embedded NUL
// survives). Use this instead of take_str for binary-safe codecs where a
// strlen would truncate at the first NUL byte.
inline std::string_view take_sv(JitValue v) {
  return v.tag == TAG_STRING
             ? _str_sv(reinterpret_cast<const char*>(v.data))
             : std::string_view{};
}
// String-or-StringView view that *enforces* the type, raising the binder's
// canonical `parameter '<name>' expects String` (the position is 0/0 and gets
// backfilled to the call site by the ns trampoline). Slow-path codecs whose
// interp twins declare a `String`-typed param use this so a non-String is a
// type error on both backends rather than silently coercing to "".
inline std::string_view require_sv(JitValue v, const char* param,
                                   const char* type_name = "String") {
  if (v.tag != TAG_STRING && v.tag != TAG_STRINGVIEW) {
    culebra::throw_runtime_error_at(
        "TypeError",
        std::format("type error: parameter '{}' expects {}", param, type_name),
        0, 0);
  }
  return _culebra_str_view(v.tag, v.data);
}
// Long/Bool twins of require_sv: enforce the declared type for a slow-path
// adapter argument, raising the binder's `parameter '<name>' expects <T>` at
// 0/0 (backfilled to the call site by the ns trampoline). Used by adapters
// reached via the as-value / resolver path, which runs no compile-side type
// check — see _ns_json_stringify / _ns_json_parse.
inline int64_t require_long(JitValue v, const char* param) {
  if (v.tag != TAG_LONG) {
    culebra::throw_runtime_error_at(
        "TypeError",
        std::format("type error: parameter '{}' expects Long", param), 0, 0);
  }
  return v.data;
}
inline bool require_bool(JitValue v, const char* param) {
  if (v.tag != TAG_BOOL) {
    culebra::throw_runtime_error_at(
        "TypeError",
        std::format("type error: parameter '{}' expects Bool", param), 0, 0);
  }
  return v.data != 0;
}
inline JitArray* take_array(JitValue v) {
  return v.tag == TAG_ARRAY ? reinterpret_cast<JitArray*>(v.data) : nullptr;
}
inline JitTensor* take_tensor(JitValue v) {
  return v.tag == TAG_TENSOR ? reinterpret_cast<JitTensor*>(v.data) : nullptr;
}
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
inline JitValue v_tensor(JitTensor* t)   {
  return {TAG_TENSOR, reinterpret_cast<int64_t>(t)};
}

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
inline JitValue _ns_io_read_all(JitValue*, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_read_all());
}
inline JitValue _ns_io_eputs(JitValue* a, int64_t) {
  culebra_runtime_eputs(a[0].tag, a[0].data);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_io_eprint(JitValue* a, int64_t) {
  culebra_runtime_eprint(a[0].tag, a[0].data);
  return _ns_adapt::v_nil();
}
// Per-stream terminal detection (POSIX isatty). Slow-path only (no fast-path
// branch / runtime helper / declare_runtime — like GC.stat): reached through
// the kNsMethods trampoline on both JIT and AOT. Matches the interp's
// IO.*_is_terminal returning a plain Bool.
inline JitValue _ns_io_stdin_is_terminal(JitValue*, int64_t) {
  return _ns_adapt::v_bool(isatty(STDIN_FILENO));
}
inline JitValue _ns_io_stdout_is_terminal(JitValue*, int64_t) {
  return _ns_adapt::v_bool(isatty(STDOUT_FILENO));
}
inline JitValue _ns_io_stderr_is_terminal(JitValue*, int64_t) {
  return _ns_adapt::v_bool(isatty(STDERR_FILENO));
}

// Bare global builtins (`puts`/`print` reuse the IO adapters above)
// exposed as first-class values: `let f = type_of`, `[1,2,3].map(type_of)`.
// Each delegates to the very runtime helper the direct-call fast path
// (compile_global) emits, so the produced value is byte-identical. line/col
// are 0 — the value/HOF call site can't thread a position through the closure
// ABI, matching the ns-method adapters above.
inline JitValue _ns_global_type_of(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_type_of(a[0].tag));
}
inline JitValue _ns_global_to_long(JitValue* a, int64_t) {
  return culebra_runtime_to_long_any(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_global_to_float(JitValue* a, int64_t) {
  return culebra_runtime_to_float_any(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_global_to_string(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_value_to_display(a[0].tag, a[0].data));
}
inline JitValue _ns_global_hash(JitValue* a, int64_t) {
  return _ns_adapt::v_long(
      culebra_runtime_hash_any(a[0].tag, a[0].data, 0, 0));
}

// range/iota as first-class values, via the args-rest NsParamMeta (kRangeMeta
// / kIotaMeta): the resolver/trampoline hand a canonical slab whose first slot
// is the positional-args Array and (for range) whose second slot is `step`.
// The adapter validates the 1-2 arg count — same ArityError the direct-call
// fast path emits — and remaps 1 arg to {0, n} / 2 args to {start, end}. Args
// are strict-Long (never Float): same TypeError wording as the fast path's
// value_to_long, though the position falls back to 0:0 since the NsMethod
// adapter ABI carries no call-site line/col.
inline int64_t _range_arg_long(JitValue v) {
  if (v.tag != TAG_LONG) culebra_runtime_type_error_typed(0, 0, "Long", v.tag);
  return v.data;
}
// Resolve {start, end} from the collected positionals Array (1 arg → {0, n}).
inline void _range_bounds(JitValue argsArray, const char* name,
                          int64_t& start, int64_t& end) {
  auto* arr = reinterpret_cast<JitArray*>(argsArray.data);
  int64_t cnt = arr->size;
  if (cnt < 1 || cnt > 2)
    throw_runtime_error_at("ArityError",
        builtin_arity_error_message(name, 1, 2, cnt), 0, 0);
  start = cnt == 2 ? _range_arg_long(arr->items[0]) : 0;
  end = _range_arg_long(arr->items[cnt - 1]);
}
inline JitValue _ns_global_range(JitValue* a, int64_t) {
  int64_t start, end;
  _range_bounds(a[0], "range", start, end);
  int64_t step = _range_arg_long(a[1]);
  return _ns_adapt::v_object(culebra_runtime_math_range(start, end, step, 0, 0));
}
inline JitValue _ns_global_iota(JitValue* a, int64_t) {
  int64_t start, end;
  _range_bounds(a[0], "iota", start, end);
  return _ns_adapt::v_array(culebra_runtime_iota(start, end));
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
inline JitValue _ns_math_sin(JitValue* a, int64_t) {
  return culebra_runtime_math_sin(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_cos(JitValue* a, int64_t) {
  return culebra_runtime_math_cos(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_tan(JitValue* a, int64_t) {
  return culebra_runtime_math_tan(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_asin(JitValue* a, int64_t) {
  return culebra_runtime_math_asin(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_acos(JitValue* a, int64_t) {
  return culebra_runtime_math_acos(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_atan(JitValue* a, int64_t) {
  return culebra_runtime_math_atan(a[0].tag, a[0].data, 0, 0);
}
inline JitValue _ns_math_atan2(JitValue* a, int64_t) {
  return culebra_runtime_math_atan2(a[0].tag, a[0].data, a[1].tag, a[1].data, 0,
                                    0);
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
                                         a[1].tag, a[1].data, 0, 0);
}
inline JitValue _ns_random_gauss(JitValue* a, int64_t) {
  return culebra_runtime_random_gauss(a[0].tag, a[0].data,
                                       a[1].tag, a[1].data, 0, 0);
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
        "Proc: env values must be String", line, col);
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

#if defined(CULEBRA_HTTP_ENABLED)
namespace _http_adapt {
// Fill `headers`/`timeout`/`follow_redirects`/`params` from the slab into
// `req`. Layout from `base`: headers, timeout, follow_redirects, into (read by
// the caller — needs streaming state), params. A non-Object/non-nil headers or
// params value, or a non-String value within them, is a TypeError.
inline void common(JitValue* a, int64_t n, int base,
                   culebra::http::HttpRequest& req, const char* ctx) {
  JitValue h = _proc_adapt::at(a, n, base);
  if (h.tag == TAG_OBJECT) {
    if (!_ns_env_object_pairs(reinterpret_cast<JitObject*>(h.data),
                              req.headers)) {
      throw culebra::CulebraError("TypeError",
          std::format("{}: header values must be String", ctx), 0, 0);
    }
  } else if (h.tag != TAG_NIL) {
    throw culebra::CulebraError("TypeError",
        std::format("{}: headers must be an Object of String", ctx), 0, 0);
  }
  JitValue to = _proc_adapt::at(a, n, base + 1);
  // interp reads timeout via the strict Value::to_long(): a present non-Long
  // (Float/String) raises `expected Long, got <T>` rather than silently
  // coercing to 0. Nil is "absent" here — the positional trampoline pads
  // missing optional slots with nil, so nil must stay equivalent to the
  // default 0. (0/0 → backfilled to the call site.)
  if (to.tag != TAG_LONG && to.tag != TAG_NIL) {
    culebra::throw_type_mismatch("Long", _culebra_tag_name(to.tag), 0, 0);
  }
  req.timeout_sec = (to.tag == TAG_LONG && to.data > 0) ? to.data : 0;
  JitValue fr = _proc_adapt::at(a, n, base + 2);
  // interp reads follow_redirects via the strict Value::to_bool() (default
  // true): Bool/Long/Float are truthy, anything else raises `expected Bool,
  // Long, or Float, got <T>`. Nil is "absent" → the default true (the
  // positional trampoline pads missing optional slots with nil). Don't use the
  // releasing _extract_bool helper — the slab owns `fr` and frees it later.
  if (fr.tag == TAG_NIL) {
    req.follow_redirects = true;
  } else if (fr.tag == TAG_BOOL || fr.tag == TAG_LONG) {
    req.follow_redirects = fr.data != 0;
  } else if (fr.tag == TAG_FLOAT) {
    req.follow_redirects = _culebra_float_to_double(fr.data) != 0.0;
  } else {
    culebra::throw_type_mismatch("Bool, Long, or Float",
                                 _culebra_tag_name(fr.tag), 0, 0);
  }
  JitValue p = _proc_adapt::at(a, n, base + 4);  // base+3 is `into` (caller)
  if (p.tag == TAG_OBJECT) {
    if (!_ns_env_object_pairs(reinterpret_cast<JitObject*>(p.data),
                              req.params)) {
      throw culebra::CulebraError("TypeError",
          std::format("{}: param values must be String", ctx), 0, 0);
    }
  } else if (p.tag != TAG_NIL) {
    throw culebra::CulebraError("TypeError",
        std::format("{}: params must be an Object of String", ctx), 0, 0);
  }
}
}  // namespace _http_adapt

// get/delete/head — slab: url, headers, timeout, follow_redirects, into.
inline JitValue _ns_http_bodyless(JitValue* a, int64_t n, const char* method,
                                  const char* ctx) {
  culebra::http::HttpRequest req;
  req.method = method;
  req.url = _ns_adapt::require_sv(a[0], "url");
  _http_adapt::common(a, n, 1, req, ctx);
  JitHttpInto st;
  _http_setup_into(_proc_adapt::at(a, n, 4), req, st, ctx);
  return _http_run_into(req, st, ctx);
}
inline JitValue _ns_http_get(JitValue* a, int64_t n) {
  return _ns_http_bodyless(a, n, "GET", "Http.get");
}
inline JitValue _ns_http_delete(JitValue* a, int64_t n) {
  return _ns_http_bodyless(a, n, "DELETE", "Http.delete");
}
inline JitValue _ns_http_head(JitValue* a, int64_t n) {
  return _ns_http_bodyless(a, n, "HEAD", "Http.head");
}

// post/put — slab: url, body, content_type, headers, timeout,
// follow_redirects, into.
inline JitValue _ns_http_withbody(JitValue* a, int64_t n, const char* method,
                                  const char* ctx) {
  culebra::http::HttpRequest req;
  req.method = method;
  req.url = _ns_adapt::require_sv(a[0], "url");
  _http_adapt::common(a, n, 3, req, ctx);
  JitHttpInto st;
  _http_setup_body(_proc_adapt::at(a, n, 1), _proc_adapt::at(a, n, 8),
                   _proc_adapt::at(a, n, 9), _proc_adapt::at(a, n, 2), req, st,
                   ctx);
  _http_setup_into(_proc_adapt::at(a, n, 6), req, st, ctx);
  return _http_run_into(req, st, ctx);
}
inline JitValue _ns_http_post(JitValue* a, int64_t n) {
  return _ns_http_withbody(a, n, "POST", "Http.post");
}
inline JitValue _ns_http_put(JitValue* a, int64_t n) {
  return _ns_http_withbody(a, n, "PUT", "Http.put");
}

// request — slab: method, url, body, content_type, headers, timeout,
// follow_redirects, into.
inline JitValue _ns_http_request(JitValue* a, int64_t n) {
  culebra::http::HttpRequest req;
  req.method = _ns_adapt::require_sv(a[0], "method");
  req.url = _ns_adapt::require_sv(a[1], "url");
  _http_adapt::common(a, n, 4, req, "Http.request");
  JitHttpInto st;
  _http_setup_body(_proc_adapt::at(a, n, 2), _proc_adapt::at(a, n, 9),
                   _proc_adapt::at(a, n, 10), _proc_adapt::at(a, n, 3), req, st,
                   "Http.request");
  _http_setup_into(_proc_adapt::at(a, n, 7), req, st, "Http.request");
  return _http_run_into(req, st, "Http.request");
}

// sse — slab: url, on_event, headers, timeout, follow_redirects. A GET that
// streams a text/event-stream response and calls on_event with each event
// Object {event, data, id}. Mirrors interp Http.sse. common() at base 2 reads
// headers@2/timeout@3/follow@4 (its params@base+4 lands out of slab → nil).
inline JitValue _ns_http_sse(JitValue* a, int64_t n) {
  const char* ctx = "Http.sse";
  culebra::http::HttpRequest req;
  req.method = "GET";
  req.url = _ns_adapt::require_sv(a[0], "url");
  if (a[1].tag != TAG_FUNC) {  // interp's on_event is a typed Function param
    culebra::throw_runtime_error_at(
        "TypeError", "type error: parameter 'on_event' expects Function", 0, 0);
  }
  auto* cb = reinterpret_cast<JitClosure*>(a[1].data);  // on_event Function
  _http_adapt::common(a, n, 2, req, ctx);
  bool has_accept = false;
  for (auto& [k, v] : req.headers) {
    if (culebra::http::_iequals(k, "Accept")) has_accept = true;
  }
  if (!has_accept) req.headers.emplace_back("Accept", "text/event-stream");
  std::exception_ptr eptr;
  culebra::http::SseDecoder dec;
  dec.handler = [cb, &eptr](const culebra::http::SseEvent& e) -> bool {
    try {
      auto* ev = culebra_runtime_object_new();
      culebra_runtime_object_set(ev, "event", false, TAG_STRING,
          reinterpret_cast<int64_t>(_culebra_heap_str(e.type)), 0, 0);
      culebra_runtime_object_set(ev, "data", false, TAG_STRING,
          reinterpret_cast<int64_t>(_culebra_heap_str(e.data)), 0, 0);
      culebra_runtime_object_set(ev, "id", false, TAG_STRING,
          reinterpret_cast<int64_t>(_culebra_heap_str(e.id)), 0, 0);
      JitValue r = _culebra_invoke1(cb, {TAG_OBJECT,
                                         reinterpret_cast<int64_t>(ev)});
      _culebra_value_release_impl(r.tag, r.data);
      return true;
    } catch (...) {
      eptr = std::current_exception();  // abort; rethrown after send
      return false;
    }
  };
  req.body_sink = [&dec](const char* d, size_t s) { return dec.feed(d, s); };
  auto r = culebra::http::http_request(req);
  if (eptr) std::rethrow_exception(eptr);
  if (!r.ok) {
    throw culebra::CulebraError("HttpError", std::format("{}: {}", ctx, r.error),
                                0, 0);
  }
  return {TAG_OBJECT,
          reinterpret_cast<int64_t>(_culebra_http_result_to_object(r, 0, 0))};
}
#endif  // CULEBRA_HTTP_ENABLED

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

// Signal.notify(tx) / Signal.reset() — JIT mirror of make_signal_namespace.
inline JitValue _ns_signal_notify(JitValue* a, int64_t n) {
  bool ok = n >= 1 && a[0].tag == TAG_OBJECT;
  JitObject* o = ok ? reinterpret_cast<JitObject*>(a[0].data) : nullptr;
  if (!ok ||
      o->find_slot("__channel_endpoint__") == static_cast<size_t>(-1) ||
      o->slots[o->find_slot("__channel_role__")].value.data != 0) {
    throw culebra::CulebraError(
        "TypeError", "Signal.notify: argument must be a channel tx endpoint", 0,
        0);
  }
  signal_notify_register(o->slots[o->find_slot("__channel_id__")].value.data);
  return {TAG_NIL, 0};
}
inline JitValue _ns_signal_reset(JitValue*, int64_t) {
  signal_notify_reset();
  return {TAG_NIL, 0};
}

// SharedBuffer.new(count, Cls): allocate a zero-initialized byte store of
// `count` @packable records and return a buffer handle. Slow-path only (no
// compile_ns_call fast path); the adapter builds everything directly. See
// [[project_packable_c3]].
// Shared.new(value): freeze through the Sendable serializer (jit side)
// and hand back the root view. Slow-path only, like the other isolate-
// family namespaces. Rejections release the in-flight refs the
// serializer took before throwing.
inline JitValue _ns_shared_new(JitValue* a, int64_t n) {
  if (n != 1) {
    throw culebra::CulebraError("ArityError",
        "Shared.new: expected 1 argument (value)");
  }
  JitSerCtx ctx;
  sendable::SendNode root;
  try {
    // Freeze mode: skip the in-flight bumps (see make_shared_namespace).
    sendable::FreezeGuard _fz;
    root = jit_serialize(a[0], ctx);
  } catch (const culebra::CulebraError& e) {
    throw culebra::CulebraError(e.kind,
        std::format("Shared.new: {}", e.what()), e.line, e.col);
  }
  if (auto* why = culebra::_shared_val_reject_reason(root)) {
    throw culebra::CulebraError("SendError",
        std::format("Shared.new: {}", why));
  }
  long id = culebra::freeze_shared_val(std::move(root));
  return _jit_make_shared_val_view(id, 0);
}

inline JitValue _ns_sharedbuffer_new(JitValue* a, int64_t n) {
  if (n != 2) {
    throw culebra::CulebraError("ArityError",
        "SharedBuffer.new: expected 2 arguments (count, Class)");
  }
  if (a[0].tag != TAG_LONG) {
    // Match interp's `count.to_long()`, which raises the canonical
    // `type error: expected Long, got <T>` (position backfilled by dispatch).
    culebra::throw_type_mismatch("Long", _culebra_tag_name(a[0].tag), 0, 0);
  }
  long count = a[0].data;
  if (count < 0) {
    throw culebra::CulebraError("ValueError",
        "SharedBuffer.new: count must be >= 0");
  }
  if (a[1].tag != TAG_OBJECT) {
    throw culebra::CulebraError("TypeError",
        "SharedBuffer.new: second argument must be a @packable class");
  }
  auto* cls = reinterpret_cast<JitObject*>(a[1].data);
  auto mi = cls->find_slot("__packable__");
  if (mi == static_cast<size_t>(-1) || cls->slots[mi].value.tag != TAG_STRING) {
    throw culebra::CulebraError("TypeError",
        "SharedBuffer.new: second argument must be a @packable class");
  }
  std::string_view cname =
      _str_sv(reinterpret_cast<const char*>(cls->slots[mi].value.data));
  const auto* layout = culebra::lookup_packable_layout(cname);
  if (!layout) {
    throw culebra::CulebraError("TypeError",
        std::format("SharedBuffer.new: no @packable layout for `{}`", cname));
  }
  long id = culebra::make_shared_buffer(*layout, std::string(cname),
                                        static_cast<size_t>(count));
  return _jit_make_shared_buffer_handle(id, count);
}

// SharedBuffer.file(path, count, Cls): file-backed mmap (persistent). Mirrors
// _ns_sharedbuffer_new + the interp `file` method.
inline JitValue _ns_sharedbuffer_file(JitValue* a, int64_t n) {
  if (n != 3) {
    throw culebra::CulebraError("ArityError",
        "SharedBuffer.file: expected 3 arguments (path, count, Class)");
  }
  if (a[0].tag != TAG_STRING) {
    throw culebra::CulebraError("TypeError",
        "SharedBuffer.file: path must be a String");
  }
  std::string path(_str_sv(reinterpret_cast<const char*>(a[0].data)));
  if (a[1].tag != TAG_LONG) {
    culebra::throw_type_mismatch("Long", _culebra_tag_name(a[1].tag), 0, 0);
  }
  long count = a[1].data;
  if (count < 0) {
    throw culebra::CulebraError("ValueError",
        "SharedBuffer.file: count must be >= 0");
  }
  if (a[2].tag != TAG_OBJECT) {
    throw culebra::CulebraError("TypeError",
        "SharedBuffer.file: type must be a @packable class");
  }
  auto* cls = reinterpret_cast<JitObject*>(a[2].data);
  auto mi = cls->find_slot("__packable__");
  if (mi == static_cast<size_t>(-1) || cls->slots[mi].value.tag != TAG_STRING) {
    throw culebra::CulebraError("TypeError",
        "SharedBuffer.file: type must be a @packable class");
  }
  std::string_view cname =
      _str_sv(reinterpret_cast<const char*>(cls->slots[mi].value.data));
  const auto* layout = culebra::lookup_packable_layout(cname);
  if (!layout) {
    throw culebra::CulebraError("TypeError",
        std::format("SharedBuffer.file: no @packable layout for `{}`", cname));
  }
  long id = culebra::make_shared_buffer_file(*layout, std::string(cname),
                                             static_cast<size_t>(count), path);
  return _jit_make_shared_buffer_handle(id, count);
}

// SharedBuffer.shared(count, Cls): anonymous fd-backed RAM, shareable with a
// child process via Proc.run `share:`. Mirrors _ns_sharedbuffer_new.
inline JitValue _ns_sharedbuffer_shared(JitValue* a, int64_t n) {
  if (n != 2) {
    throw culebra::CulebraError("ArityError",
        "SharedBuffer.shared: expected 2 arguments (count, Class)");
  }
  if (a[0].tag != TAG_LONG) {
    culebra::throw_type_mismatch("Long", _culebra_tag_name(a[0].tag), 0, 0);
  }
  long count = a[0].data;
  if (count < 0) {
    throw culebra::CulebraError("ValueError",
        "SharedBuffer.shared: count must be >= 0");
  }
  if (a[1].tag != TAG_OBJECT) {
    throw culebra::CulebraError("TypeError",
        "SharedBuffer.shared: second argument must be a @packable class");
  }
  auto* cls = reinterpret_cast<JitObject*>(a[1].data);
  auto mi = cls->find_slot("__packable__");
  if (mi == static_cast<size_t>(-1) || cls->slots[mi].value.tag != TAG_STRING) {
    throw culebra::CulebraError("TypeError",
        "SharedBuffer.shared: second argument must be a @packable class");
  }
  std::string_view cname =
      _str_sv(reinterpret_cast<const char*>(cls->slots[mi].value.data));
  const auto* layout = culebra::lookup_packable_layout(cname);
  if (!layout) {
    throw culebra::CulebraError("TypeError",
        std::format("SharedBuffer.shared: no @packable layout for `{}`", cname));
  }
  long id = culebra::make_shared_buffer_shared(*layout, std::string(cname),
                                               static_cast<size_t>(count));
  return _jit_make_shared_buffer_handle(id, count);
}

// SharedBuffer.receive(name, Cls): child side — mmap the buffer the parent
// passed via Proc.run `share:`. `count` is the parent's (read from the env).
inline JitValue _ns_sharedbuffer_receive(JitValue* a, int64_t n) {
  if (n != 2) {
    throw culebra::CulebraError("ArityError",
        "SharedBuffer.receive: expected 2 arguments (name, Class)");
  }
  if (a[0].tag != TAG_STRING) {
    throw culebra::CulebraError("TypeError",
        "SharedBuffer.receive: name must be a String");
  }
  std::string_view name = _str_sv(reinterpret_cast<const char*>(a[0].data));
  if (a[1].tag != TAG_OBJECT) {
    throw culebra::CulebraError("TypeError",
        "SharedBuffer.receive: second argument must be a @packable class");
  }
  auto* cls = reinterpret_cast<JitObject*>(a[1].data);
  auto mi = cls->find_slot("__packable__");
  if (mi == static_cast<size_t>(-1) || cls->slots[mi].value.tag != TAG_STRING) {
    throw culebra::CulebraError("TypeError",
        "SharedBuffer.receive: second argument must be a @packable class");
  }
  std::string_view cname =
      _str_sv(reinterpret_cast<const char*>(cls->slots[mi].value.data));
  const auto* layout = culebra::lookup_packable_layout(cname);
  if (!layout) {
    throw culebra::CulebraError("TypeError",
        std::format("SharedBuffer.receive: no @packable layout for `{}`", cname));
  }
  long id = culebra::make_shared_buffer_from_share_env(*layout,
                                                       std::string(cname), name);
  auto core = culebra::lookup_shared_buffer(id);
  return _jit_make_shared_buffer_handle(
      id, core ? static_cast<long>(core->count) : 0);
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

// JSON.stringify(v, indent=0, sort_keys=false, lines=false): the resolver
// fills a full 4-slot slab (defaults for omitted params); a bare positional
// value-call (`let f = JSON.stringify; f(x)`) passes a 1..4 prefix, so read
// each optional slot only when present. Each typed param is validated here so
// the as-value path (which runs no compile-side check) rejects a wrong type
// with the interp binder's wording; a direct call's compile-side positional
// check fires first at the argument's position.
inline JitValue _ns_json_stringify(JitValue* a, int64_t n) {
  int64_t indent = (n > 1) ? _ns_adapt::require_long(a[1], "indent") : 0;
  int8_t sort_keys =
      (n > 2) ? (_ns_adapt::require_bool(a[2], "sort_keys") ? 1 : 0) : 0;
  int8_t lines = (n > 3) ? (_ns_adapt::require_bool(a[3], "lines") ? 1 : 0) : 0;
  return _ns_adapt::v_string(
      culebra_runtime_json_stringify(a[0].tag, a[0].data, indent, sort_keys,
                                     lines));
}
// JSON.parse(s, lines=false, number_mode="auto"): same variable-prefix slab.
inline JitValue _ns_json_parse(JitValue* a, int64_t n) {
  _ns_adapt::require_sv(a[0], "s");  // s: String
  int8_t lines = (n > 1) ? (_ns_adapt::require_bool(a[1], "lines") ? 1 : 0) : 0;
  std::string mode =
      (n > 2) ? std::string(_ns_adapt::require_sv(a[2], "number_mode")) : "auto";
  if (mode != "auto" && mode != "float") {
    // interp validates this in the method body (a ValueError, not a typed-param
    // TypeError); 0/0 backfills to the call site like the interp's position.
    culebra::throw_runtime_error_at(
        "ValueError", "JSON.parse: number_mode must be 'auto' or 'float'", 0, 0);
  }
  return culebra_runtime_json_parse(_ns_adapt::take_str(a[0]), mode.c_str(),
                                     lines);
}

// Encoding.html.{escape,unescape}: the codec logic is shared with interp via
// shared.h. Slow-path only (nested namespaces bypass compile_ns_call), so
// these are reached through the kNsMethods closure trampoline.
inline JitValue _ns_encoding_html_escape(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      _culebra_heap_str(culebra::html_escape(_ns_adapt::require_sv(a[0], "s"))));
}
inline JitValue _ns_encoding_html_unescape(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      _culebra_heap_str(culebra::html_unescape(_ns_adapt::require_sv(a[0], "s"))));
}
// Encoding.base64.{encode,decode}: shared codec; decode raises ValueError on
// invalid input (same as interp). take_sv (not take_str) keeps the codecs
// binary-safe — embedded NUL bytes must not truncate the input.
inline JitValue _ns_encoding_base64_encode(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      _culebra_heap_str(culebra::base64_encode(_ns_adapt::require_sv(a[0], "s"))));
}
inline JitValue _ns_encoding_base64_decode(JitValue* a, int64_t) {
  auto r = culebra::base64_decode(_ns_adapt::require_sv(a[0], "s"));
  if (!r) {
    throw culebra::CulebraError("ValueError",
        "Encoding.base64.decode: invalid base64", 0, 0);
  }
  return _ns_adapt::v_string(_culebra_heap_str(*r));
}
// Encoding.hex.{encode,decode}: shared codec; decode raises ValueError on
// invalid input (same as interp).
inline JitValue _ns_encoding_hex_encode(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      _culebra_heap_str(culebra::hex_encode(_ns_adapt::require_sv(a[0], "s"))));
}
inline JitValue _ns_encoding_hex_decode(JitValue* a, int64_t) {
  auto r = culebra::hex_decode(_ns_adapt::require_sv(a[0], "s"));
  if (!r) {
    throw culebra::CulebraError("ValueError",
        "Encoding.hex.decode: invalid hex", 0, 0);
  }
  return _ns_adapt::v_string(_culebra_heap_str(*r));
}
// Encoding.url.{encode,decode}: percent-encoding; decode is lenient (never
// fails), matching interp.
inline JitValue _ns_encoding_url_encode(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      _culebra_heap_str(culebra::url_encode(_ns_adapt::require_sv(a[0], "s"))));
}
inline JitValue _ns_encoding_url_decode(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      _culebra_heap_str(culebra::url_decode(_ns_adapt::require_sv(a[0], "s"))));
}

// Compress.{gzip,gunzip}: shared zlib logic via compress.h; gunzip raises
// ValueError on malformed input (same message as interp). require_sv keeps the
// data binary-safe (embedded NUL must not truncate). Slow-path only.
inline JitValue _ns_compress_gzip(JitValue* a, int64_t) {
  auto r = culebra::compress::gzip(_ns_adapt::require_sv(a[0], "data"));
  if (!r.error.empty()) {
    throw culebra::CulebraError("ValueError", "Compress.gzip: " + r.error, 0, 0);
  }
  return _ns_adapt::v_string(_culebra_heap_str(r.data));
}
inline JitValue _ns_compress_gunzip(JitValue* a, int64_t) {
  auto r = culebra::compress::gunzip(_ns_adapt::require_sv(a[0], "data"));
  if (!r.error.empty()) {
    throw culebra::CulebraError("ValueError", "Compress.gunzip: " + r.error, 0,
                                0);
  }
  return _ns_adapt::v_string(_culebra_heap_str(r.data));
}

// Hash.{sha256,sha1,sha512,md5} + Hash.hmac_*: self-hosted digests via hash.h,
// returning the lowercase hex digest. require_sv keeps the input binary-safe
// and raises the binder's canonical `parameter '<name>' expects String` for a
// non-String argument (matching interp). Slow-path only.
inline JitValue _ns_hash_sha256(JitValue* a, int64_t) {
  return _ns_adapt::v_string(_culebra_heap_str(
      culebra::hashing::sha256(_ns_adapt::require_sv(a[0], "data"))));
}
inline JitValue _ns_hash_sha1(JitValue* a, int64_t) {
  return _ns_adapt::v_string(_culebra_heap_str(
      culebra::hashing::sha1(_ns_adapt::require_sv(a[0], "data"))));
}
inline JitValue _ns_hash_sha512(JitValue* a, int64_t) {
  return _ns_adapt::v_string(_culebra_heap_str(
      culebra::hashing::sha512(_ns_adapt::require_sv(a[0], "data"))));
}
inline JitValue _ns_hash_md5(JitValue* a, int64_t) {
  return _ns_adapt::v_string(_culebra_heap_str(
      culebra::hashing::md5(_ns_adapt::require_sv(a[0], "data"))));
}
inline JitValue _ns_hash_hmac_sha256(JitValue* a, int64_t) {
  return _ns_adapt::v_string(_culebra_heap_str(culebra::hashing::hmac_sha256(
      _ns_adapt::require_sv(a[0], "key"), _ns_adapt::require_sv(a[1], "data"))));
}
inline JitValue _ns_hash_hmac_sha1(JitValue* a, int64_t) {
  return _ns_adapt::v_string(_culebra_heap_str(culebra::hashing::hmac_sha1(
      _ns_adapt::require_sv(a[0], "key"), _ns_adapt::require_sv(a[1], "data"))));
}
inline JitValue _ns_hash_hmac_sha512(JitValue* a, int64_t) {
  return _ns_adapt::v_string(_culebra_heap_str(culebra::hashing::hmac_sha512(
      _ns_adapt::require_sv(a[0], "key"), _ns_adapt::require_sv(a[1], "data"))));
}

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

// --- The dispatch table ---

//===-- Regex: the `Regex` namespace functions. Like GC.stat, no fast-path
// branch or runtime helper is needed — these slow-path adapters do the work
// and build the JitObject/JitArray results directly. All take (pattern,
// subject, ...); flags are inline ((?i)/(?m)/(?s)). A Match is a data object
// { value, start, end, groups:[Group|nil], named:{name:Group} }; no-match nil.
//===------------------------------------------------------------------------//
inline std::shared_ptr<regexlib::Regex> _jit_regex_compile(std::string_view pat) {
  // Stateless cache keyed by pattern (own thread-local; see the /simplify note
  // about sharing with stdlib_interp's regex_compile_cached).
  static thread_local std::unordered_map<std::string,
                                         std::shared_ptr<regexlib::Regex>>
      cache;
  std::string p(pat);
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
  o->is_match = true;  // route `m[i]` / `m["name"]` to capture groups
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
  _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));  // validate; throws on bad pattern
  return _ns_adapt::v_nil();
}
inline JitValue _ns_regex_test(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  return _ns_adapt::v_bool(re->test(_ns_adapt::require_sv(a[1], "s", "StringLike")));
}
inline JitValue _ns_regex_find(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  auto m = re->search(_ns_adapt::require_sv(a[1], "s", "StringLike"));
  return m.matched ? _jit_regex_match(m) : _ns_adapt::v_nil();
}
inline JitValue _ns_regex_match(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  auto m = re->match(_ns_adapt::require_sv(a[1], "s", "StringLike"));
  return m.matched ? _jit_regex_match(m) : _ns_adapt::v_nil();
}
inline JitValue _ns_regex_find_all(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  auto* arr = culebra_runtime_array_new();
  for (auto& m : re->find_all(_ns_adapt::require_sv(a[1], "s", "StringLike"))) {
    auto mv = _jit_regex_match(m);
    culebra_runtime_array_push(arr, mv.tag, mv.data);
  }
  return _ns_adapt::v_array(arr);
}
// find_all_str(pattern, s) -> [String]: matched texts only, no Match objects
// (the per-match Object structure dominated match-dense workloads). See interp.
inline JitValue _ns_regex_find_all_str(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  auto* arr = culebra_runtime_array_new();
  for (auto& m : re->find_all(_ns_adapt::require_sv(a[1], "s", "StringLike"))) {
    culebra_runtime_array_push(
        arr, TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(m.str)));
  }
  return _ns_adapt::v_array(arr);
}
// count(pattern, s) -> Long: number of non-overlapping matches, no objects.
inline JitValue _ns_regex_count(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  return JitValue{TAG_LONG, static_cast<int64_t>(
                                re->find_all(_ns_adapt::require_sv(a[1], "s", "StringLike")).size())};
}
// find_all_index(pattern, s) -> [Int]: flat byte spans [s0,e0,s1,e1,...].
// Longs are inline in the JitArray, so the whole result is one allocation.
inline JitValue _ns_regex_find_all_index(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  auto* arr = culebra_runtime_array_new();
  for (auto& m : re->find_all(_ns_adapt::require_sv(a[1], "s", "StringLike"))) {
    culebra_runtime_array_push(arr, TAG_LONG, static_cast<int64_t>(m.begin));
    culebra_runtime_array_push(arr, TAG_LONG, static_cast<int64_t>(m.end));
  }
  return _ns_adapt::v_array(arr);
}
inline JitValue _ns_regex_replace_all(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  std::string out = re->replace_all(_ns_adapt::require_sv(a[1], "s", "StringLike"),
                                    _ns_adapt::require_sv(a[2], "repl", "StringLike"));
  return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(out))};
}
// find_from(pattern, s, pos) -> { m: Match|nil, next: Int }. See the interp
// twin in stdlib_interp.h: leftmost match at/after byte `pos` with absolute
// offsets, plus the grapheme-correct resume position.
inline JitValue _ns_regex_find_from(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  std::string s(_ns_adapt::require_sv(a[1], "s", "StringLike"));
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
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  std::string s(_ns_adapt::require_sv(a[1], "s", "StringLike"));
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

// Per-parameter view for stdlib methods that accept kwargs / have defaults.
// Derived once (per process) from the canonical interp FunctionValue::Parameter
// list — never hand-authored — so the JIT binder and the interp binder cannot
// drift. `name`/`type` are string_views into the canonical params (stable for
// the process); `canon_default` points at the canonical default Value and is
// converted fresh to a JitValue per use (so heap defaults like "" stay
// per-call owned). Built by `_ns_meta` from the canonical params.
struct NsParam {
  std::string_view name;
  bool has_default;
  bool kw_only;
  const culebra::Value* canon_default;  // null iff !has_default
  // Declared type of a *required* leading positional ("String" / "Function" /
  // "Array"); empty for defaulted params. When set, the compile side emits an
  // inline type check at the argument's source position (matching interp's
  // binder) instead of letting the runtime adapter reject it positionlessly.
  std::string_view type;
};

struct NsParamMeta {
  const NsParam* params;
  int n_params;
  int kwargs_rest_idx;    // -1 = none
  int first_kw_only_idx;  // -1 = none
  // Index of the positional catch-all (`*args`) slot, or -1. All positional
  // arguments collect into an Array here (range/iota: the 1-2 start/end args),
  // letting a method mix variadic positionals with keyword-only params the
  // way the interp binder does. Assumes no regular positional param precedes
  // it (true for range/iota). Default-initialized so existing aggregate
  // initializers (which omit it) keep -1.
  int args_rest_idx = -1;
  // Minimum required positional arity, computed once at derivation by the
  // shared `culebra::builtin_arity_bounds` (the single source of the arity
  // rule, also used by the interp binder) — not recomputed per call.
  int min_arity = 0;
  // Maximum positional arity (positional-bindable param count), from the same
  // `builtin_arity_bounds`. The resolver checks `n_pos` against [min,max] up
  // front — before merging splats/kwargs — exactly as the interp's strict_arity
  // block does, so kwargs/splats never satisfy a required positional and the
  // arity error fires at the same point on both backends.
  int max_arity = 0;
};

struct NsMethod {
  const char* ns;
  const char* name;
  int8_t arity;  // -1 = variadic
  JitValue (*adapter)(JitValue* args, int64_t n);
  // Non-null = method lives in a nested sub-namespace object on the parent
  // (e.g. `Encoding.html.unescape` has ns="Encoding", sub="html"). Nested
  // namespaces are slow-path only — they never reach compile_ns_call. The
  // kwarg/default spec (NsParamMeta) is derived from the canonical interp
  // params by `_ns_meta` — see _ns_method_uses_kwarg_slab — not stored here.
  const char* sub = nullptr;
  // Declared type + interp param name of the (single) leading positional, for
  // methods that don't use a kwarg slab (e.g. FS.read: arg0_name="path",
  // arg0_type="String"; Tensor.from: "a"/"Array"). Lets the compile side emit
  // an inline check at the argument position, and `_jit_ns_method_dispatch`
  // check the as-value / HOF path (where there is no inline check).
  const char* arg0_type = nullptr;
  const char* arg0_name = nullptr;
};

// --- Single source of truth for the calling convention -----------------------
// The JIT binder's per-parameter view (NsParamMeta / NsParam) is DERIVED, once
// per process, from the canonical interp FunctionValue::Parameter list — never
// hand-authored — so the interp and JIT binders cannot drift. Only the choice
// of *which* methods use a kwarg/default slab (a JIT codegen-strategy decision,
// see _ns_method_uses_kwarg_slab) is JIT-side; the param data all flows from
// the canonical spec.

// A fresh JitValue for a canonical stdlib default Value. Stdlib defaults are
// scalars (nil / bool / long / float / string); heap strings are re-allocated
// per call so each binding owns its own +1.
inline JitValue _jit_default_from_value(const culebra::Value& v) {
  switch (v.type) {
    case culebra::Value::Nil:    return {TAG_NIL, 0};
    case culebra::Value::Bool:   return {TAG_BOOL, v.get<bool>() ? 1 : 0};
    case culebra::Value::Long:   return {TAG_LONG, v.get<long>()};
    case culebra::Value::Float:  return jit_float(v.get<double>());
    case culebra::Value::String:
      return {TAG_STRING,
              reinterpret_cast<int64_t>(_culebra_heap_str(v.to_string()))};
    default:
      throw culebra::CulebraError("InternalError",
          "unsupported stdlib default value", 0, 0);
  }
}

// The canonical interp environment, built once. Param specs are immutable, so a
// single shared instance serves every isolate / thread. environment() is a
// standalone factory available at JIT compile time, JIT runtime, and inside the
// AOT runtime archive (culebra_rt.cc includes stdlib_interp.h).
inline const culebra::Environment& _canonical_env() {
  static const std::shared_ptr<culebra::Environment> env =
      culebra::environment();
  return *env;
}

// Resolve an NsMethod to its canonical interp parameter list, or null. Handles
// namespace methods (Ns.method), nested sub-namespace methods (Ns.sub.method),
// and bare globals (ns == "" → e.g. range / iota in the global dictionary).
inline const std::vector<culebra::FunctionValue::Parameter>*
_canon_params(const NsMethod* m) {
  const auto& env = _canonical_env();
  const culebra::Value* fnv = nullptr;
  if (m->ns == nullptr || m->ns[0] == '\0') {
    auto it = env.dictionary.find(std::string(m->name));
    if (it != env.dictionary.end()) fnv = &it->second.val;
  } else {
    auto it = env.dictionary.find(std::string(m->ns));
    if (it == env.dictionary.end() ||
        it->second.val.type != culebra::Value::Object) return nullptr;
    const auto& ns_obj = it->second.val.to_object();
    if (m->sub != nullptr) {
      if (!ns_obj.has(m->sub)) return nullptr;
      const auto& sub_v = ns_obj.get(m->sub);
      if (sub_v.type != culebra::Value::Object) return nullptr;
      const auto& sub_obj = sub_v.to_object();
      if (!sub_obj.has(m->name)) return nullptr;
      fnv = &sub_obj.get(m->name);
    } else {
      if (!ns_obj.has(m->name)) return nullptr;
      fnv = &ns_obj.get(m->name);
    }
  }
  if (fnv == nullptr || fnv->type != culebra::Value::Function) return nullptr;
  return fnv->to_function().params.get();
}

// Whether this method's JIT adapter consumes a kwarg/default slab (built by the
// shared resolver) rather than taking raw positional args. This is a JIT-adapter
// contract, NOT derivable from the canonical spec: several raw-adapter methods
// (Channel.fan_in, Isolate.spawn, the File handle's read, …) also carry
// canonical defaults / *args yet must keep their raw argv. So the *membership*
// of this set is listed explicitly; the per-parameter DATA for the members is
// still derived from the canonical params by `_ns_meta` (the single source).
inline bool _ns_method_uses_kwarg_slab(const NsMethod* m) {
  std::string_view ns(m->ns ? m->ns : ""), nm(m->name);
  if (ns == "Proc")     return nm == "run" || nm == "all" || nm == "spawn" ||
                                nm == "race";
  if (ns == "FS")       return nm == "remove" || nm == "copy";
  if (ns == "File")     return nm == "open" || nm == "with";
  if (ns == "Parallel") return nm == "map" || nm == "each" ||
                                nm == "map_settled" || nm == "race";
  if (ns == "Http")     return true;  // all Http methods take kwargs
  if (ns == "JSON")     return nm == "stringify" || nm == "parse";
  if (ns.empty())       return nm == "range" || nm == "iota";  // bare globals
  return false;
}

// Derived param spec for a method, or null if it doesn't use a kwarg slab.
// Built once per process; lock-free reads after init (see definition below
// kBuiltinFns, where the method tables it walks are in scope).
inline const NsParamMeta* _ns_meta(const NsMethod* m);

// Per-positional TYPE view for every method (not just kwarg-slab ones). Used by
// the closure trampoline to type-check each positional at the argument's source
// position, matching the interp binder. See definition below kBuiltinFns.
inline const NsParamMeta* _ns_type_meta(const NsMethod* m);

// NsMethod rows for wrap.h-declared classes (wrapped_ns_rows), built
// lazily once — after static-init froze the registry, so the c_str
// pointers into its strings are stable — and merged into every table
// consumer below alongside the static kNsMethods rows. The class name
// rides in `sub` (a nested `Ns.Class.method`, slow-path only, the
// Encoding.html shape), and the param spec still derives from the
// CANONICAL interp params via _canon_params: the interp env builds the
// same namespaces from the same registry, so the calling-convention
// single source covers generated bindings unchanged.
inline const std::vector<NsMethod>& _wrapped_ns_methods() {
  static const std::vector<NsMethod> rows = [] {
    std::vector<NsMethod> v;
    v.reserve(culebra::wrapped_ns_rows().size());
    for (const auto& r : culebra::wrapped_ns_rows()) {
      v.push_back(NsMethod{
          r.ns.c_str(), r.name.c_str(), r.arity, r.adapter, r.sub.c_str(),
          r.arg0_type.empty() ? nullptr : r.arg0_type.c_str(),
          r.arg0_name.empty() ? nullptr : r.arg0_name.c_str()});
    }
    return v;
  }();
  return rows;
}

inline const NsMethod kNsMethods[] = {
  {"IO",     "puts",      1, &_ns_io_puts},
  {"IO",     "print",     1, &_ns_io_print},
  {"IO",     "input",     0, &_ns_io_input},
  {"IO",     "read_all",  0, &_ns_io_read_all},
  {"IO",     "eputs",     1, &_ns_io_eputs},
  {"IO",     "eprint",    1, &_ns_io_eprint},
  {"IO",     "stdin_is_terminal",  0, &_ns_io_stdin_is_terminal},
  {"IO",     "stdout_is_terminal", 0, &_ns_io_stdout_is_terminal},
  {"IO",     "stderr_is_terminal", 0, &_ns_io_stderr_is_terminal},

  {"Math",   "abs",       1, &_ns_math_abs},
  {"Math",   "log",       1, &_ns_math_log},
  {"Math",   "exp",       1, &_ns_math_exp},
  {"Math",   "sqrt",      1, &_ns_math_sqrt},
  {"Math",   "sin",       1, &_ns_math_sin},
  {"Math",   "cos",       1, &_ns_math_cos},
  {"Math",   "tan",       1, &_ns_math_tan},
  {"Math",   "asin",      1, &_ns_math_asin},
  {"Math",   "acos",      1, &_ns_math_acos},
  {"Math",   "atan",      1, &_ns_math_atan},
  {"Math",   "atan2",     2, &_ns_math_atan2},
  {"Math",   "floor",     1, &_ns_math_floor},
  {"Math",   "ceil",      1, &_ns_math_ceil},
  {"Math",   "round",     1, &_ns_math_round},
  {"Math",   "min",      -1, &_ns_math_min},
  {"Math",   "max",      -1, &_ns_math_max},
  {"Math",   "pow",       2, &_ns_math_pow},
  {"Math",   "sign",      1, &_ns_math_sign},
  {"Math",   "clamp",     3, &_ns_math_clamp},

  {"FS",     "join",     -1, &_ns_fs_join},
  {"FS",     "basename",  1, &_ns_fs_basename, nullptr, "String", "path"},
  {"FS",     "dirname",   1, &_ns_fs_dirname, nullptr, "String", "path"},
  {"FS",     "extension", 1, &_ns_fs_extension, nullptr, "String", "path"},
  {"FS",     "stem",      1, &_ns_fs_stem, nullptr, "String", "path"},
  {"FS",     "exists",    1, &_ns_fs_exists, nullptr, "String", "path"},
  {"FS",     "is_file",   1, &_ns_fs_is_file, nullptr, "String", "path"},
  {"FS",     "is_dir",    1, &_ns_fs_is_dir, nullptr, "String", "path"},
  {"FS",     "read",      1, &_ns_fs_read, nullptr, "String", "path"},
  {"FS",     "write",     2, &_ns_fs_write},
  {"FS",     "size",      1, &_ns_fs_size, nullptr, "String", "path"},
  {"FS",     "list_dir",  1, &_ns_fs_list_dir, nullptr, "String", "path"},
  {"FS",     "mkdir",     1, &_ns_fs_mkdir},
  {"FS",     "remove",    1, &_ns_fs_remove},
  {"FS",     "stat",      1, &_ns_fs_stat},
  {"FS",     "rename",    2, &_ns_fs_rename},
  {"FS",     "copy",      2, &_ns_fs_copy},
  {"FS",     "normpath",  1, &_ns_fs_normpath},
  {"FS",     "is_abs",    1, &_ns_fs_is_abs},
  {"FS",     "abspath",   1, &_ns_fs_abspath},
  {"FS",     "realpath",  1, &_ns_fs_realpath},
  {"FS",     "is_symlink",1, &_ns_fs_is_symlink},
  {"FS",     "symlink",   2, &_ns_fs_symlink},
  {"FS",     "readlink",  1, &_ns_fs_readlink},
  {"FS",     "walk",      1, &_ns_fs_walk},
  {"FS",     "glob",      1, &_ns_fs_glob},

  {"File",   "open",      1, &_ns_file_open},
  {"File",   "with",      2, &_ns_file_with},

  {"Random", "seed",            1, &_ns_random_seed},
  {"Random", "int",             2, &_ns_random_int},
  {"Random", "uniform",         2, &_ns_random_uniform},
  {"Random", "gauss",           2, &_ns_random_gauss},
  {"Random", "shuffle",         1, &_ns_random_shuffle, nullptr, "Array", "a"},
  {"Random", "weighted_choice", 2, &_ns_random_weighted_choice},

  {"Sys",    "exit", 1, &_ns_sys_exit},
  {"Sys",    "env",  1, &_ns_sys_env, nullptr, "String", "name"},
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

  {"Proc",   "run",   1, &_ns_proc_run},
  {"Proc",   "all",   1, &_ns_proc_all},
  {"Proc",   "race",  1, &_ns_proc_race},
  {"Proc",   "spawn", 1, &_ns_proc_spawn},

#if defined(CULEBRA_HTTP_ENABLED)
  {"Http",   "get",     1, &_ns_http_get},
  {"Http",   "delete",  1, &_ns_http_delete},
  {"Http",   "head",    1, &_ns_http_head},
  {"Http",   "post",    1, &_ns_http_post},
  {"Http",   "put",     1, &_ns_http_put},
  {"Http",   "request", 2, &_ns_http_request},
  {"Http",   "sse",     2, &_ns_http_sse},
#endif

  {"Isolate", "spawn", -1, &_ns_isolate_spawn},
  {"Channel", "new",    -1, &_ns_channel_new},
  {"Channel", "fan_in", -1, &_ns_channel_fan_in},
  {"Signal",  "notify", 1, &_ns_signal_notify},
  {"Signal",  "reset",  0, &_ns_signal_reset},
  {"SharedBuffer", "new", 2, &_ns_sharedbuffer_new},
  {"Shared", "new", 1, &_ns_shared_new},
  {"SharedBuffer", "file", 3, &_ns_sharedbuffer_file},
  {"SharedBuffer", "shared", 2, &_ns_sharedbuffer_shared},
  {"SharedBuffer", "receive", 2, &_ns_sharedbuffer_receive},
  {"Parallel", "map",         2, &_ns_parallel_map},
  {"Parallel", "each",        2, &_ns_parallel_each},
  {"Parallel", "map_settled", 2, &_ns_parallel_map_settled},
  {"Parallel", "race",        2, &_ns_parallel_race},

  {"JSON",   "stringify", 1, &_ns_json_stringify},
  {"JSON",   "parse",     1, &_ns_json_parse, nullptr, "String", "s"},

  // Nested sub-namespace (sub="html"): reached only via bare-resolve +
  // member access, e.g. `Encoding.html.unescape(s)`.
  {"Encoding", "escape",   1, &_ns_encoding_html_escape, "html",   "String", "s"},
  {"Encoding", "unescape", 1, &_ns_encoding_html_unescape, "html",   "String", "s"},
  {"Encoding", "encode",   1, &_ns_encoding_base64_encode, "base64", "String", "s"},
  {"Encoding", "decode",   1, &_ns_encoding_base64_decode, "base64", "String", "s"},
  {"Encoding", "encode",   1, &_ns_encoding_hex_encode, "hex",    "String", "s"},
  {"Encoding", "decode",   1, &_ns_encoding_hex_decode, "hex",    "String", "s"},
  {"Encoding", "encode",   1, &_ns_encoding_url_encode, "url",    "String", "s"},
  {"Encoding", "decode",   1, &_ns_encoding_url_decode, "url",    "String", "s"},

  {"Compress", "gzip",     1, &_ns_compress_gzip,   nullptr, "String", "data"},
  {"Compress", "gunzip",   1, &_ns_compress_gunzip, nullptr, "String", "data"},

  {"Hash", "sha256",      1, &_ns_hash_sha256, nullptr, "String", "data"},
  {"Hash", "sha1",        1, &_ns_hash_sha1,   nullptr, "String", "data"},
  {"Hash", "sha512",      1, &_ns_hash_sha512, nullptr, "String", "data"},
  {"Hash", "md5",         1, &_ns_hash_md5,    nullptr, "String", "data"},
  {"Hash", "hmac_sha256", 2, &_ns_hash_hmac_sha256, nullptr, "String", "key"},
  {"Hash", "hmac_sha1",   2, &_ns_hash_hmac_sha1,   nullptr, "String", "key"},
  {"Hash", "hmac_sha512", 2, &_ns_hash_hmac_sha512, nullptr, "String", "key"},

  {"Tensor", "zeros",    -1, &_ns_tensor_zeros},
  {"Tensor", "ones",     -1, &_ns_tensor_ones},
  {"Tensor", "randn",    -1, &_ns_tensor_randn},
  {"Tensor", "from",      1, &_ns_tensor_from, nullptr, "Array",  "a"},
  {"Tensor", "from_csv",  1, &_ns_tensor_from_csv, nullptr, "String", "path"},
  {"Tensor", "eval",     -1, &_ns_tensor_eval},
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

// Build the canonical slab for an args-rest method (range/iota): every
// positional collects into a fresh Array at `args_rest_idx` (these methods
// have no regular positional param before it), and each remaining slot fills
// from `merged` kwargs (moved out) or its default. Mirrors the interp binder
// for `range(*args, step=1)`, so the bare-positional trampoline and the
// kwarg/splat resolver hand the adapter an identical slab. The caller owns +1
// on every positional and every value in `merged`; ownership transfers into
// the returned slab (released by the caller after the adapter runs). On a
// missing-required / unknown-keyword error the slab and `merged` are released
// here before throwing.
inline std::vector<JitValue> _jit_ns_build_args_rest_slab(
    const NsParamMeta* pm, int64_t n_pos, JitValue* positional,
    std::unordered_map<std::string_view, JitValue>& merged,
    int64_t line, int64_t col) {
  std::vector<JitValue> slab(pm->n_params, JitValue{TAG_NIL, 0});
  auto* arr = culebra_runtime_array_new();
  for (int64_t i = 0; i < n_pos; i++)
    culebra_runtime_array_push(arr, positional[i].tag, positional[i].data);
  slab[pm->args_rest_idx] = {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
  auto cleanup = [&]() {
    for (auto& sv : slab) _culebra_value_release_impl(sv.tag, sv.data);
    for (auto& [_, v] : merged) _culebra_value_release_impl(v.tag, v.data);
  };
  for (int i = 0; i < pm->n_params; i++) {
    if (i == pm->args_rest_idx) continue;
    auto it = merged.find(pm->params[i].name);
    if (it != merged.end()) {
      slab[i] = it->second;
      merged.erase(it);
    } else if (pm->params[i].has_default) {
      slab[i] = _jit_default_from_value(*pm->params[i].canon_default);
    } else {
      cleanup();
      throw culebra::CulebraError("ArityError",
          std::format("missing required argument '{}'", pm->params[i].name),
          line, col);
    }
  }
  if (!merged.empty()) {
    auto bad = std::string(merged.begin()->first);
    cleanup();
    throw culebra::CulebraError("TypeError",
        std::format("unknown keyword argument '{}'", bad), line, col);
  }
  return slab;
}

// Run an args-rest method's adapter over its canonical (caller-owned) slab,
// always releasing the slab afterward — including if the adapter throws.
// Shared by the bare-positional trampoline and the kwarg/splat resolver.
inline JitValue _jit_ns_dispatch_owned_slab(const NsMethod* m,
                                            std::vector<JitValue>& slab) {
  // The slab lives in a std::vector's heap buffer, which the conservative
  // collector does not scan (it walks the machine stack + registered globals
  // only). So its GC values — e.g. range/iota's positionals Array — are
  // unreachable from any root across the adapter's own allocations; a
  // GC_STRESS collect would sweep them, and the release() below would then
  // touch freed memory. Pause collection for the dispatch.
  culebra::gc::Heap::CollectPause _pause(_gc_heap());
  auto release = [&]() {
    for (auto& sv : slab) _culebra_value_release_impl(sv.tag, sv.data);
  };
  try {
    auto r = m->adapter(slab.data(), _ns_meta(m)->n_params);
    release();
    return r;
  } catch (...) {
    release();
    throw;
  }
}

// Core ns-method dispatch: arity-check `n_args` against `m`, run the adapter
// (consuming/releasing `args`), and backfill a positionless error to
// (line,col). Shared by the closure trampoline (which snapshots the call site
// from the thread-local) and the `NsMethod*`-driven entry used by
// compile_ns_call for Http / the nested Encoding codecs (which pass the
// argument/call position explicitly).
inline JitValue _jit_ns_method_dispatch(const NsMethod* m, int64_t n_args,
                                        JitValue* args, int64_t line,
                                        int64_t col, int64_t arg0_line,
                                        int64_t arg0_col) {
  const NsParamMeta* pm = _ns_meta(m);
  try {
    // Args-rest method (range/iota): build the same canonical
    // [rest-Array, kw-only...] slab the kwarg resolver produces, so a single
    // adapter shape serves both. The positionals are absorbed into the Array,
    // so the slab (not `args`) is what gets released afterward.
    if (pm && pm->args_rest_idx >= 0) {
      std::unordered_map<std::string_view, JitValue> none;
      auto slab = _jit_ns_build_args_rest_slab(pm, n_args, args, none,
                                               line, col);
      return _jit_ns_dispatch_owned_slab(m, slab);
    }
    auto release_args = [&]() {
      for (int64_t i = 0; i < n_args; i++) {
        _culebra_value_release_impl(args[i].tag, args[i].data);
      }
    };
    // Methods with NsParamMeta accept a variable arity (required params up to
    // the full param count); the kwarg resolver fills a full-arity slab and
    // bare positional calls pass a prefix. Methods without params keep the
    // strict fixed-arity check.
    if (pm) {
      if (n_args < pm->min_arity || n_args > pm->max_arity) {
        release_args();
        // Too few → required count; too many → the cap (interp parity).
        _ns_adapt::arity_error(
            m->ns, m->name,
            n_args > pm->max_arity ? pm->max_arity : pm->min_arity, n_args,
            line, col);
      }
    } else if (m->arity >= 0 && n_args != m->arity) {
      release_args();
      _ns_adapt::arity_error(m->ns, m->name, m->arity, n_args, line, col);
    }
    // As-value / HOF path: a param-less method's lenient adapter would coerce a
    // wrong-typed leading positional (FS.read(5) → IOError, Tensor.from(5) →
    // empty), so reject it first. interp doesn't run a declared-type binder for
    // a callback — the method body's coercion fails with `Value::_throw_type_error`
    // ("type error: expected <T>, got <actual>"); mirror that exact wording here
    // (not the binder's "parameter '<name>' expects <T>", which the syntactic
    // direct-call form emits inline in compile_ns_call). Reported at the
    // argument's position (arg0_line/col, threaded from the indirect call site).
    if (!pm && m->arg0_type != nullptr && n_args >= 1 &&
        !_culebra_type_matches_single(args[0].tag, args[0].data,
                                      m->arg0_type)) {
      const char* got = culebra_runtime_type_of(args[0].tag);
      release_args();
      culebra::throw_runtime_error_at(
          "TypeError",
          std::format("type error: expected {}, got {}", m->arg0_type, got),
          arg0_line, arg0_col);
    }
    try {
      auto r = m->adapter(args, n_args);
      release_args();
      return r;
    } catch (...) {
      release_args();
      throw;
    }
  } catch (culebra::CulebraError& e) {
    if (e.line == 0) { e.line = line; e.col = col; }
    throw;
  }
}

inline JitValue _jit_ns_method_trampoline(
    JitClosure* cls, JitValue /*this_val*/, int64_t n_args, JitValue* args) {
  const auto* m = reinterpret_cast<const NsMethod*>(
      cls->captures[0]->value.data);
  // As-value call (`let g = ns.method; g(...)`): the indirect-call codegen
  // threaded each argument's source position into _jit_argpos_*. Type-check the
  // positionals against the method's declared param types with the interp
  // binder's wording ("parameter '<name>' expects <T>") at the argument's
  // position — exactly like a direct call. A HOF callback leaves the count 0,
  // so this is skipped and the dispatch's body-coercion arg0 check runs below
  // (matching the interp's callback wording). Args-rest methods (range/iota)
  // bind positionals into the rest Array, so they keep the dispatch path.
  if (_jit_argpos_n > 0) {
    // `_ns_type_meta` carries every method's per-positional type (derived from
    // the canonical params), so all positionals — not just arg0 — are checked
    // here, matching the interp binder. Args-rest methods (range/iota) bind
    // positionals into the rest Array, so skip them (the dispatch validates).
    const NsParamMeta* tm = _ns_type_meta(m);
    for (int64_t i = 0; i < n_args && i < _jit_argpos_n; i++) {
      std::string_view ty, pname;
      if (tm) {
        if (tm->args_rest_idx < 0 && i < tm->n_params) {
          ty = tm->params[i].type;
          pname = tm->params[i].name;
        }
      } else if (i == 0 && m->arg0_type) {  // canonical lookup failed: arg0 only
        ty = m->arg0_type;
        pname = m->arg0_name ? m->arg0_name : "";
      }
      if (!ty.empty() &&
          !_culebra_value_matches_type(args[i].tag, args[i].data, ty)) {
        for (int64_t k = 0; k < n_args; k++)
          _culebra_value_release_impl(args[k].tag, args[k].data);
        culebra::throw_runtime_error_at(
            "TypeError",
            std::format("type error: parameter '{}' expects {}", pname, ty),
            _jit_argpos_line[i], _jit_argpos_col[i]);
      }
    }
  }
  // The closure ABI carries no line/col, so snapshot the positions recorded
  // just before this indirect call. Pass the call site for arity errors and
  // arg0's position for the (callback) body-coercion arg0 type check.
  return _jit_ns_method_dispatch(m, n_args, args, _jit_call_site_line,
                                 _jit_call_site_col, _jit_call_arg0_line,
                                 _jit_call_arg0_col);
}

inline JitClosure* _jit_make_ns_method_closure(const NsMethod* m) {
  _jit_register_native_fn(
      reinterpret_cast<const void*>(&_jit_ns_method_trampoline));
  // Atomic w.r.t. collection: the capture cell is registered before `cls`
  // itself is, so a GC_STRESS collect mid-build would find the cell
  // unreachable (its only ref is the not-yet-registered cls) and sweep it.
  culebra::gc::Heap::CollectPause _pause(_gc_heap());
  auto* cls = new JitClosure();
  cls->refcount = 1;
  cls->fn_ptr = reinterpret_cast<void*>(_jit_ns_method_trampoline);
  cls->n_captures = 1;
  cls->captures = new JitCell*[1];
  cls->captures[0] = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(m));
  cls->arity = m->arity < 0 ? JIT_VARIADIC_ARITY : static_cast<size_t>(m->arity);
  _gc_register(cls, GC_TAG_FUNC);
  return cls;
}

// Resolve a kwarg/splat call against an ns-method `m` carrying NsParamMeta.
// Mirrors culebra_runtime_call_with_kwargs' merge order (splat first, explicit
// kwargs override) but fills missing defaulted slots with the param's real
// default value (the C++ adapters have no callee prologue to expand a
// TAG_UNFILLED sentinel). Builds a full-arity positional slab and dispatches
// through `_jit_ns_method_dispatch`. Consumes positional/kwarg/splat values.
// Returns false (without consuming) when `m` has no NsParamMeta.
inline bool _jit_ns_kwarg_resolve_core(
    const NsMethod* m, int64_t n_pos, JitValue* positional,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs, int64_t line, int64_t col,
    JitValue* out) {
  const NsParamMeta* pm = _ns_meta(m);
  if (!pm) return false;  // method doesn't take kwargs

  auto release_all = [&]() {
    for (int64_t i = 0; i < n_pos; i++)
      _culebra_value_release_impl(positional[i].tag, positional[i].data);
    for (int64_t i = 0; i < n_kw; i++)
      _culebra_value_release_impl(kw_vals[i].tag, kw_vals[i].data);
    for (int64_t i = 0; i < n_splat; i++)
      _culebra_value_release_impl(splat_objs[i].tag, splat_objs[i].data);
  };

  // Arity check on positional count — up front, before merging splats/kwargs,
  // mirroring the interp's strict_arity block (invoke_user_function_with_args):
  // a required positional is never satisfied by a keyword/splat, and the
  // ArityError fires before any splat type check. Variadic (args_rest) methods
  // carry no positional cap (handled below), so skip — matching interp's
  // `if (!b.variadic)`. Too few reports the required count, too many the cap.
  if (pm->args_rest_idx < 0 &&
      (n_pos < pm->min_arity || n_pos > pm->max_arity)) {
    release_all();
    throw culebra::CulebraError("ArityError",
        culebra::ns_fn_arity_error_message(
            n_pos < pm->min_arity ? pm->min_arity : pm->max_arity, n_pos),
        line, col);
  }

  // Merge splats then explicit kwargs (each owns +1 in `merged`).
  std::unordered_map<std::string_view, JitValue> merged;
  for (int64_t i = 0; i < n_splat; i++) {
    if (splat_objs[i].tag != TAG_OBJECT) {
      release_all();
      throw culebra::CulebraError("TypeError", std::format(
          "**: splat operand must be Object, got {}",
          _culebra_tag_name(splat_objs[i].tag)), line, col);
    }
    auto* obj = reinterpret_cast<JitObject*>(splat_objs[i].data);
    if (obj->non_string_props && !obj->non_string_props->empty()) {
      release_all();
      throw culebra::CulebraError("TypeError",
          "**: splat key must be String", line, col);
    }
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

  // Args-rest method (range/iota): positionals collect into the rest Array
  // instead of binding fixed slots, so the variadic 1-2 start/end args coexist
  // with the keyword-only `step`. The shared builder yields the same canonical
  // slab the bare-positional trampoline does; dispatch the adapter directly
  // (the slab is already canonical) and release it after.
  if (pm->args_rest_idx >= 0) {
    auto slab = _jit_ns_build_args_rest_slab(pm, n_pos, positional, merged,
                                             line, col);
    try {
      *out = _jit_ns_dispatch_owned_slab(m, slab);
    } catch (culebra::CulebraError& e) {
      if (e.line == 0) { e.line = line; e.col = col; }
      throw;
    }
    return true;
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
                      "keyword", pm->params[i].name),
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
    _ns_adapt::arity_error(m->ns, m->name, n, n_pos, line, col);
  }
  // Remaining params: from merged kwargs, else default, else ArityError.
  for (int i = static_cast<int>(n_pos); i < n; i++) {
    auto it = merged.find(pm->params[i].name);
    if (it != merged.end()) {
      slab[i] = it->second;
      filled[i] = true;
      merged.erase(it);
    } else if (pm->params[i].has_default) {
      slab[i] = _jit_default_from_value(*pm->params[i].canon_default);
      filled[i] = true;
    } else {
      for (int k = 0; k < n; k++)
        if (filled[k]) _culebra_value_release_impl(slab[k].tag, slab[k].data);
      for (auto& [_, v] : merged)
        _culebra_value_release_impl(v.tag, v.data);
      // Count-based message (required count, got positionals) — the shared
      // canonical form interp's binder uses, so `Http.get()` /
      // `Http.request("GET")` etc. report identically across backends.
      throw culebra::CulebraError("ArityError",
          culebra::ns_fn_arity_error_message(pm->min_arity, n_pos), line, col);
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
        std::format("unknown keyword argument '{}'", bad), line, col);
  }

  // Dispatch the full-arity slab (it releases the slab values, and backfills a
  // positionless adapter error from the passed call site — matching interp's
  // call-site position for adapter-internal type errors like
  // `Http.get(params: 5)`; the typed-positional checks already fired earlier at
  // the argument position).
  // pm is non-null here, so the dispatch's arg0 type check is inert; pass the
  // call site for arg0 (the typed positionals were already checked inline).
  *out = _jit_ns_method_dispatch(m, n, slab.data(), line, col, line, col);
  return true;
}

// Closure-ABI wrapper: extract the NsMethod from the closure and resolve.
// Returns false (the hook's "not mine" signal) for non-ns closures.
inline bool _jit_ns_kwarg_resolve(
    JitClosure* cls, JitValue /*this_val*/, int64_t n_pos, JitValue* positional,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs, int64_t line, int64_t col,
    JitValue* out) {
  if (cls->fn_ptr != reinterpret_cast<void*>(_jit_ns_method_trampoline)) {
    return false;
  }
  const auto* m = reinterpret_cast<const NsMethod*>(
      cls->captures[0]->value.data);
  return _jit_ns_kwarg_resolve_core(m, n_pos, positional, n_kw, kw_keys,
                                    kw_vals, n_splat, splat_objs, line, col,
                                    out);
}

// Callback-arity bounds for an ns-method closure handed to a HOF. Derives
// (cb_min, cb_max) from the SAME canonical params interp's check_callback_arity
// reads (builtin_arity_bounds over _canon_params), so both backends gate an
// ns-method callback identically. Returns false for non-ns closures.
inline bool _jit_ns_callback_arity(JitClosure* cls, long* cb_min, long* cb_max) {
  if (cls->fn_ptr != reinterpret_cast<void*>(_jit_ns_method_trampoline)) {
    return false;
  }
  const auto* m = reinterpret_cast<const NsMethod*>(
      cls->captures[0]->value.data);
  const auto* cps = _canon_params(m);
  if (!cps) return false;  // no canonical params → fall back to closure arity
  auto b = culebra::builtin_arity_bounds(*cps);
  *cb_min = b.min;
  *cb_max = b.variadic ? -1 : b.max;
  return true;
}

// Install the kwarg + callback-arity hooks once, before any JIT call runs.
inline const bool _jit_ns_kwarg_hook_installed = [] {
  _jit_ns_kwarg_hook = &_jit_ns_kwarg_resolve;
  _jit_ns_callback_arity_hook = &_jit_ns_callback_arity;
  return true;
}();

inline JitObject* _jit_build_namespace_object(std::string_view ns_name) {
  // Build with collection paused: the method closures and sub-namespace
  // objects are registered but not reachable from any root until they are
  // slotted, so a GC_STRESS collect mid-build would sweep them.
  culebra::gc::Heap::CollectPause _pause(_gc_heap());
  auto* obj = culebra_runtime_object_new();
  // Sub-namespace objects, created lazily and keyed by `sub` (e.g. "html").
  // Reachable from `obj` (a pinned root), so the marker keeps them + their
  // method closures alive for the program's lifetime.
  std::unordered_map<std::string_view, JitObject*> subs;
  auto add_method = [&](const NsMethod& m) {
    if (ns_name != m.ns) return;
    auto* fn = _jit_make_ns_method_closure(&m);
    JitValue fv{TAG_FUNC, reinterpret_cast<int64_t>(fn)};
    if (m.sub) {
      auto& sub = subs[m.sub];
      if (!sub) sub = culebra_runtime_object_new();
      sub->append_slot(m.name, fv, /*mut=*/false);
    } else {
      obj->append_slot(m.name, fv, /*mut=*/false);
    }
  };
  for (auto& m : kNsMethods) add_method(m);
  for (auto& m : _wrapped_ns_methods()) add_method(m);
  // Wrapped classes with no ctor/static rows still get their (empty)
  // class sub-object, mirroring the interp's registry walk.
  for (auto& wc : culebra::wrapped_classes()) {
    if (ns_name != wc.ns) continue;
    auto& sub = subs[wc.name];
    if (!sub) sub = culebra_runtime_object_new();
  }
  for (auto& [name, sub] : subs) {
    obj->append_slot(name, JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(sub)},
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
    obj->append_slot(
        "executable",
        JitValue{TAG_STRING, reinterpret_cast<int64_t>(
                                 _culebra_heap_str(
                                     culebra::current_executable_path()))},
        /*mut=*/false);
  }
  return obj;
}

struct _JitNamespaceTable {
  std::unordered_map<std::string, JitObject*> entries;
  // Bare builtin function globals (`puts`, `type_of`, `range`, ...) used
  // as first-class values, materialized lazily as ns-method closures. Same
  // lifetime/teardown order as `entries` — both hold heap values that must
  // be released before the GC heap substate (a lower slot) is destroyed.
  std::unordered_map<std::string, JitClosure*> builtin_fns;
  ~_JitNamespaceTable() {
    for (auto& [_, obj] : entries) {
      _culebra_value_release_impl(TAG_OBJECT,
                                  reinterpret_cast<int64_t>(obj));
    }
    for (auto& [_, cls] : builtin_fns) {
      _culebra_value_release_impl(TAG_FUNC,
                                  reinterpret_cast<int64_t>(cls));
    }
  }
};

// Bare builtin function globals, exposed as first-class values by reusing
// the ns-method closure machinery (shared trampoline + arity check). Direct
// calls keep their fast paths (compile_global / try_compile_core_global);
// only value / higher-order uses (`map(type_of)`, `let f = puts`) reach the
// closure built here. `ns` is empty — these are not namespaced; the trampoline
// uses it only for arity-error text, which `_ns_adapt::arity_error` renders
// name-only when blank.
inline const NsMethod kBuiltinFns[] = {
  {"", "puts",      1, &_ns_io_puts},
  {"", "print",     1, &_ns_io_print},
  {"", "type_of",   1, &_ns_global_type_of},
  {"", "to_long",   1, &_ns_global_to_long},
  {"", "to_float",  1, &_ns_global_to_float},
  {"", "to_string", 1, &_ns_global_to_string},
  {"", "hash",      1, &_ns_global_hash},
  {"", "range",    -1, &_ns_global_range},
  {"", "iota",     -1, &_ns_global_iota},
};

namespace _ns_spec_detail {
struct DerivedSpec {
  std::vector<NsParam> params;  // params.data() backs meta.params
  NsParamMeta meta;
};

// Build the derived param spec for one method from its canonical interp params,
// or null if the canonical lookup fails. The per-parameter data (names, types,
// defaults, *args/**kwargs/kw-only indices, arity bounds) is the SINGLE SOURCE
// shared by `_ns_meta` (kwarg-slab subset, drives kwarg/arity dispatch) and
// `_ns_type_meta` (every method, drives the trampoline's positional type
// checks) — so the two views can never drift from each other or from interp.
inline std::unique_ptr<DerivedSpec> build_ns_spec(const NsMethod* m) {
  const auto* cps = _canon_params(m);
  if (cps == nullptr) return nullptr;  // canonical lookup failed — skip
  auto sp = std::make_unique<DerivedSpec>();
  sp->params.reserve(cps->size());
  int kwargs_rest_idx = -1, first_kw_only_idx = -1, args_rest_idx = -1;
  for (int i = 0; i < static_cast<int>(cps->size()); i++) {
    const auto& cp = (*cps)[i];
    bool has_default = cp.default_expr != nullptr || cp.default_value != nullptr;
    sp->params.push_back(NsParam{
        cp.name, has_default, cp.kw_only,
        has_default ? cp.default_value.get() : nullptr,
        // Canonical declared type for every param. The compile side / trampoline
        // emits a runtime type check at the argument's source position for any
        // positional bound to a typed param, matching the interp binder.
        cp.type_name});
    if (cp.kwargs_rest && kwargs_rest_idx < 0) kwargs_rest_idx = i;
    if (cp.kw_only && first_kw_only_idx < 0) first_kw_only_idx = i;
    if (cp.args_rest && args_rest_idx < 0) args_rest_idx = i;
  }
  auto _ab = culebra::builtin_arity_bounds(*cps);
  sp->meta = NsParamMeta{sp->params.data(),
                         static_cast<int>(sp->params.size()),
                         kwargs_rest_idx, first_kw_only_idx, args_rest_idx,
                         static_cast<int>(_ab.min), static_cast<int>(_ab.max)};
  return sp;
}
}  // namespace _ns_spec_detail

// Build (once, lock-free reads after) the derived NsParamMeta for every method
// whose adapter consumes a kwarg/default slab, reading the canonical interp
// FunctionValue params. Specs live in `storage` (unique_ptr → stable address);
// `meta.params` points at each spec's vector buffer (sized exactly, never
// reallocated after build), and `canon_default` points into the canonical
// param's default Value (kept alive by `_canonical_env`).
inline const NsParamMeta* _ns_meta(const NsMethod* m) {
  static std::vector<std::unique_ptr<_ns_spec_detail::DerivedSpec>> storage;
  static const std::unordered_map<const NsMethod*, const NsParamMeta*> table =
      [&] {
        std::unordered_map<const NsMethod*, const NsParamMeta*> t;
        auto add = [&](const NsMethod& nm) {
          if (!_ns_method_uses_kwarg_slab(&nm)) return;
          auto sp = _ns_spec_detail::build_ns_spec(&nm);
          if (!sp) return;
          t.emplace(&nm, &sp->meta);
          storage.push_back(std::move(sp));
        };
        for (const auto& nm : kNsMethods) add(nm);
        for (const auto& nm : kBuiltinFns) add(nm);
        for (const auto& nm : _wrapped_ns_methods()) add(nm);
        return t;
      }();
  auto it = table.find(m);
  return it == table.end() ? nullptr : it->second;
}

// Per-positional type view for EVERY method (not gated by _ns_method_uses_
// kwarg_slab). The closure trampoline consults this to type-check each
// positional at the argument's source position — exactly like the interp binder
// — instead of letting a pure-positional adapter coerce a wrong-typed arg (e.g.
// `FS.rename(a, 5)` silently running rename on ""). Distinct from `_ns_meta`,
// which gates kwarg/arity DISPATCH; widening this type-only view to all methods
// cannot change dispatch. Same canonical source (build_ns_spec) → no drift.
inline const NsParamMeta* _ns_type_meta(const NsMethod* m) {
  static std::vector<std::unique_ptr<_ns_spec_detail::DerivedSpec>> storage;
  static const std::unordered_map<const NsMethod*, const NsParamMeta*> table =
      [&] {
        std::unordered_map<const NsMethod*, const NsParamMeta*> t;
        auto add = [&](const NsMethod& nm) {
          auto sp = _ns_spec_detail::build_ns_spec(&nm);
          if (!sp) return;
          t.emplace(&nm, &sp->meta);
          storage.push_back(std::move(sp));
        };
        for (const auto& nm : kNsMethods) add(nm);
        for (const auto& nm : kBuiltinFns) add(nm);
        for (const auto& nm : _wrapped_ns_methods()) add(nm);
        return t;
      }();
  auto it = table.find(m);
  return it == table.end() ? nullptr : it->second;
}

// Materialize (once, cached + pinned for the isolate's lifetime) the closure
// for a bare builtin function name, or null if `name` is not one. Mirrors
// `_jit_namespace_get_or_build`'s caching/pinning so the value can be passed
// around like any user closure without the GC reclaiming it between uses.
inline JitClosure* _jit_builtin_fn_closure(const std::string& name) {
  auto& cache = culebra::runtime_substate<_JitNamespaceTable>(
                    culebra::kSlotJitNamespaceTable).builtin_fns;
  auto it = cache.find(name);
  if (it != cache.end()) return it->second;
  for (const auto& m : kBuiltinFns) {
    if (name != m.name) continue;
    auto* cls = _jit_make_ns_method_closure(&m);
    _gc_heap().pin(cls);
    cache.emplace(name, cls);
    return cls;
  }
  return nullptr;
}

// Namespace names this dispatcher knows how to build. Kept in sync
// with interp's `setup_built_in_functions` by the drift check below.
// `_Time` is the Time-class ABI primitive — internal, not user-facing
// — and intentionally excluded.
inline bool _is_known_ns(std::string_view name) {
  for (auto& m : kNsMethods) if (name == m.ns) return true;
  for (auto& m : _wrapped_ns_methods()) if (name == m.ns) return true;
  // A wrapped class with no ctor/static still binds its (empty) class
  // object on the interp side — keep the ns known here so the drift
  // check and bare resolve stay symmetric.
  for (auto& wc : culebra::wrapped_classes()) if (name == wc.ns) return true;
  for (auto& c : kNsConstants) if (name == c.ns) return true;
  return name == "Sys";  // Sys has only constants in some configs
}

// Resolve `Ns[.sub].method` to its NsMethod, or null. `sub` is nullptr for a
// top-level method (e.g. Http.get) and the sub-namespace name for a nested one
// (e.g. Encoding.base64.encode → sub="base64"). Resolution is by name (not a
// baked pointer) so the same call works under AOT, where the kNsMethods table
// address differs from JIT-compile time.
inline const NsMethod* _lookup_ns_method(std::string_view ns,
                                         std::string_view method,
                                         const char* sub = nullptr) {
  auto match = [&](const NsMethod& m) {
    bool sub_match = sub == nullptr
                         ? m.sub == nullptr
                         : (m.sub != nullptr && std::string_view(sub) == m.sub);
    return sub_match && ns == m.ns && method == m.name;
  };
  for (auto& m : kNsMethods) if (match(m)) return &m;
  for (auto& m : _wrapped_ns_methods()) if (match(m)) return &m;
  return nullptr;
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
// One-shot namespace-coverage check: if interp's setup_built_in_functions binds
// a namespace this dispatcher doesn't know about, abort with a clear message at
// first slow-path resolve. Catches adding a new stdlib namespace (e.g. `Net`)
// to stdlib_interp.h while forgetting to add adapters + kNsMethods rows here.
// Per-parameter drift is no longer possible (NsParamMeta is derived from the
// canonical interp params by _ns_meta), so there is nothing else to verify.
inline void _check_ns_drift_once() {
  static const bool checked = []() {
    static const std::set<std::string_view> kInternalNs = {"_Time"};
    const auto& env = _canonical_env();
    for (const auto& [key, sym] : env.dictionary) {
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
  std::string nm(name ? name : "");
  // Bare builtin function used as a value (`let f = puts`, `map(type_of)`).
  // Checked before the namespace lookup since these names are not namespaces.
  if (auto* cls = _jit_builtin_fn_closure(nm)) {
    culebra_runtime_value_retain(TAG_FUNC, reinterpret_cast<int64_t>(cls));
    *out_tag = TAG_FUNC;
    *out_data = reinterpret_cast<int64_t>(cls);
    return;
  }
  auto* obj = _jit_namespace_get_or_build(nm);
  if (!obj) {
    culebra::throw_runtime_error_at(
        "NameError",
        std::format("undefined variable '{}'", name ? name : ""),
        0, 0);
  }
  culebra_runtime_value_retain(TAG_OBJECT,
                                reinterpret_cast<int64_t>(obj));
  *out_tag = TAG_OBJECT;
  *out_data = reinterpret_cast<int64_t>(obj);
}

// Direct ns-method call entry used by compile_ns_call's typed-positional fast
// path (Http + the nested Encoding codecs). The method is identified by
// ns/method/sub *names* (not a baked NsMethod* — that would be invalid under
// AOT, where the table lives at a different address) and resolved here. Routes
// through the same resolver/dispatch the closure trampoline uses, so
// kwargs/splats/defaults/arity all behave identically — only the
// typed-positional type errors have already been emitted at the argument
// position by the caller. Consumes the positional/kwarg/splat values.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_ns_method_call_kw(
    const char* ns, const char* method, const char* sub,
    int64_t n_pos, JitValue* positional,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs, int64_t line, int64_t col) {
  const NsMethod* m = _lookup_ns_method(ns, method, sub);
  JitValue out;
  if (_jit_ns_kwarg_resolve_core(m, n_pos, positional, n_kw, kw_keys, kw_vals,
                                 n_splat, splat_objs, line, col, &out)) {
    return out;
  }
  // Param-less method (no NsParamMeta, e.g. the Encoding codecs): the resolver
  // bailed without consuming. The caller passes no kwargs/splats for these, so
  // dispatch the positionals directly (dispatch consumes them). The typed arg0
  // was already checked inline at its position by the caller, so the dispatch's
  // arg0 check is inert here; pass the call site for it.
  return _jit_ns_method_dispatch(m, n_pos, positional, line, col, line, col);
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
                    rt::math_sin, rt::math_cos, rt::math_tan,
                    rt::math_asin, rt::math_acos, rt::math_atan,
                    rt::math_floor, rt::math_ceil, rt::math_round,
                    rt::math_abs}) {
    jit.module_->getOrInsertFunction(name, jit.valueType_,
                                 jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty());
  }
  // atan2(y, x): two (tag, data) pairs + line + col -> JitValue.
  jit.module_->getOrInsertFunction(rt::math_atan2, jit.valueType_,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
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
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::random_gauss, jit.valueType_,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
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

  // Iterator lazy factories. map/filter/take_while carry line+col so the
  // eager callback-arity check can report the call site (see iter_chain/zip/
  // flat_map below, which already carry them for the "not iterable" error).
  jit.module_->getOrInsertFunction(rt::iter_map, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_filter, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_take, ptrTy, i8, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_skip, ptrTy, i8, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_take_while, ptrTy, i8, i64, i8, i64,
                               i64, i64);
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
    // range/iota accept a `step` kwarg / `**` splat — fall through to nullptr
    // so the general path resolves them as their args-rest closure and routes
    // through call_with_kwargs (the resolver handles step + unknown-kwarg).
    // The genuinely positional-only globals keep their clean SyntaxError.
    static const std::set<std::string_view> kwarg_rejecting_globals = {
        "puts", "print", "to_long", "to_float", "to_string", "type_of", "hash",
    };
    if (kwarg_rejecting_globals.contains(name)) {
      // Catchable runtime TypeError matching the interp (interpreter.h
      // ~6429), not a compile-time SyntaxError — `try { puts(x: 1) }` must
      // observe the same error on both backends, at the same call-site
      // position. A duplicate/positional-after-keyword shape is caught earlier
      // by emit_arg_list_check, so this fires only for a well-formed kwarg.
      jit.emit_throw_error("TypeError",
          "function does not accept keyword arguments",
          callAst.line, callAst.column);
      return jit.make_nil();  // unreachable after the throw
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
                             std::string_view w,                              \
                             const peg::Ast* a = nullptr) {                   \
    return jit.emit_type_check(v, t, w, a);                                   \
  };                                                                          \
  auto compile = [&](const peg::Ast& a) { return jit.compile(a); };           \
  auto emit_output_call = [&](const char* rt, const peg::Ast& a) {            \
    return JitExtension::emit_output_call(jit, rt, a);                        \
  };                                                                          \
  auto emit_call = [&](llvm::FunctionCallee callee,                           \
                       llvm::ArrayRef<llvm::Value*> args) {                   \
    return jit.emit_call(callee, args);                                       \
  }

// An ARG_LIST split into positionals / explicit kwargs / **splats. The single
// AST-scan shared by every kwarg-accepting stdlib compile path (Proc / Http /
// JSON), so the merge order and the two structural errors (duplicate keyword,
// positional-after-keyword) are defined once. Those errors are NOT thrown here:
// interp reports them at runtime (catchable), so the scan records the first one
// and the caller re-emits it as a runtime IR throw — matching interp and the
// general kwargs path. `err_eval_count` is how many leading args the caller
// evaluates (for side effects) before the throw, mirroring interp's
// evaluate-then-error point: the whole list for a duplicate keyword (caught in
// the binder after a full collection), but only up to the offending arg for a
// positional-after-keyword (caught mid-collection, before that arg is run).
struct ScannedArgs {
  std::vector<const peg::Ast*> positional;
  std::vector<std::pair<std::string_view, const peg::Ast*>> explicit_kwargs;
  std::set<std::string_view> seen_explicit;
  std::vector<const peg::Ast*> splats;
  const char* err_kind = nullptr;  // "TypeError" | "SyntaxError" | nullptr
  std::string err_msg;
  long err_line = 0, err_col = 0;
  size_t err_eval_count = 0;
};

inline ScannedArgs _jit_scan_arg_list(const peg::Ast& argsAst) {
  using namespace peg::udl;
  ScannedArgs s;
  bool saw_named = false;
  for (size_t i = 0; i < argsAst.nodes.size(); i++) {
    const auto& child = argsAst.nodes[i];
    if (child->tag == "KWARG_SPLAT"_) {
      saw_named = true;
      s.splats.push_back(child->nodes[0].get());
    } else if (child->tag == "KWARG"_) {
      saw_named = true;
      auto name = child->nodes[0]->token;
      if (!s.seen_explicit.insert(name).second) {
        s.err_kind = "TypeError";
        s.err_msg = std::format("duplicate keyword argument '{}'", name);
        s.err_line = static_cast<long>(child->line);
        s.err_col = static_cast<long>(child->column);
        s.err_eval_count = argsAst.nodes.size();  // interp evals the full list
        return s;
      }
      s.explicit_kwargs.emplace_back(name, child->nodes[1].get());
    } else {
      if (saw_named) {
        s.err_kind = "SyntaxError";
        s.err_msg = "positional argument follows keyword argument";
        s.err_line = static_cast<long>(child->line);
        s.err_col = static_cast<long>(child->column);
        s.err_eval_count = i;  // interp throws before evaluating this arg
        return s;
      }
      s.positional.push_back(child.get());
    }
  }
  return s;
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
    static const std::set<std::string_view> kwarg_aware_ns = {"JSON", "Proc",
                                                              "Http"};
    if (!kwarg_aware_ns.contains(ns)) {
      return nullptr;
    }
  }
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto line = builder_.getInt64(callAst.line);
  auto col = builder_.getInt64(callAst.column);

  // Emit the canonical positional type check for argument `i` of this ns method:
  // `parameter '<name>' expects <T>` at the argument's source position, exactly
  // like the interp binder. Used by the inline fast paths (Math.pow/clamp,
  // Random.int, …) whose `value_to_long`/`value_to_*` coercions would otherwise
  // report a positionless `expected <T>, got <X>` at the call site. The name +
  // type come from the canonical params (via _ns_type_meta), so no hand-authored
  // names and no drift. No-op for an untyped param or a failed lookup.
  auto emit_canon_arg_check = [&](int i, llvm::Value* v) {
    if (const NsMethod* mm = _lookup_ns_method(ns, method)) {
      if (const NsParamMeta* tm = _ns_type_meta(mm)) {
        if (i < tm->n_params && !tm->params[i].type.empty()) {
          std::string ctx = "parameter '" + std::string(tm->params[i].name) + "'";
          emit_type_check(v, tm->params[i].type, ctx, argsAst.nodes[i].get());
        }
      }
    }
  };

  // Http.<method>(url[, method], ...kwargs): compile the call against the
  // statically-resolved NsMethod so the typed positional(s) (url/method/
  // on_event) get an inline type check at the argument position, matching
  // interp. kwargs/arity/dispatch reuse the shared resolver.
  if (ns == "Http") {
    if (const NsMethod* m = _lookup_ns_method("Http", method)) {
      return compile_ns_method_kwargs(jit, m, argsAst, callAst);
    }
  }

  // Proc.run / Proc.all route their (positional + kwargs) ARG_LIST through one
  // marshaller into the matching `_kw` runtime fn. Proc.race is positional-only
  // and falls through to the namespace-object trampoline.
  if (ns == "Proc" && method == "run") {
    return compile_single_positional_kwargs(
        jit, argsAst, callAst, "Proc.run", rt::proc_run_kw,
        _ns_meta(_lookup_ns_method("Proc", "run")));
  }
  if (ns == "Proc" && method == "all") {
    return compile_single_positional_kwargs(
        jit, argsAst, callAst, "Proc.all", rt::proc_all_kw,
        _ns_meta(_lookup_ns_method("Proc", "all")));
  }
  if (ns == "Proc" && method == "spawn") {
    return compile_single_positional_kwargs(
        jit, argsAst, callAst, "Proc.spawn", rt::proc_spawn_kw,
        _ns_meta(_lookup_ns_method("Proc", "spawn")));
  }
  // Proc.race(commands, share:) — same kwarg-aware path as run/all/spawn, so
  // `commands` gets the typed-`Array` message + argument position and `share:`
  // is honored. The runtime keeps its backstop checks.
  if (ns == "Proc" && method == "race") {
    return compile_single_positional_kwargs(
        jit, argsAst, callAst, "Proc.race", rt::proc_race_kw,
        _ns_meta(_lookup_ns_method("Proc", "race")));
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
    if (method == "sin")   { if (auto v = math_unary_runtime(rt::math_sin))   return v; }
    if (method == "cos")   { if (auto v = math_unary_runtime(rt::math_cos))   return v; }
    if (method == "tan")   { if (auto v = math_unary_runtime(rt::math_tan))   return v; }
    if (method == "asin")  { if (auto v = math_unary_runtime(rt::math_asin))  return v; }
    if (method == "acos")  { if (auto v = math_unary_runtime(rt::math_acos))  return v; }
    if (method == "atan")  { if (auto v = math_unary_runtime(rt::math_atan))  return v; }
    if (method == "atan2" && argsAst.nodes.size() == 2) {
      auto y = compile(*argsAst.nodes[0]);
      auto x = compile(*argsAst.nodes[1]);
      auto r = emit_call(module_->getFunction(rt::math_atan2),
                         {extract_tag(y), extract_data(y), extract_tag(x),
                          extract_data(x), line, col});
      emit_value_release(y);
      emit_value_release(x);
      return r;
    }

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
      // >=1 numeric arg (max(5) == 5). n==1 takes the general slab path
      // below (the n==2 branch is just a no-alloca fast path).
      if (n < 1) return nullptr;
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
      auto baseV = compile(*argsAst.nodes[0]); emit_canon_arg_check(0, baseV);
      auto expV = compile(*argsAst.nodes[1]); emit_canon_arg_check(1, expV);
      auto base = value_to_long(baseV);
      auto exp = value_to_long(expV);
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
      auto xV = compile(*argsAst.nodes[0]); emit_canon_arg_check(0, xV);
      auto loV = compile(*argsAst.nodes[1]); emit_canon_arg_check(1, loV);
      auto hiV = compile(*argsAst.nodes[2]); emit_canon_arg_check(2, hiV);
      auto x = value_to_long(xV);
      auto lo = value_to_long(loV);
      auto hi = value_to_long(hiV);
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
      emit_type_check(arg, "String", "parameter 'path'", argsAst.nodes[0].get());
      auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto s = emit_call(module_->getFunction(rt::read_file),
                         {ptr, line, col});
      emit_value_release(arg);
      return make_string(s);
    }
    if (method == "write" && argsAst.nodes.size() == 2) {
      auto p = compile(*argsAst.nodes[0]);
      emit_type_check(p, "String", "parameter 'path'", argsAst.nodes[0].get());
      auto c = compile(*argsAst.nodes[1]);
      emit_type_check(c, "String", "parameter 'content'",
                      argsAst.nodes[1].get());
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
      emit_type_check(arg, "String", "parameter 'path'", argsAst.nodes[0].get());
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
      emit_type_check(arg, "String", "parameter 'path'", argsAst.nodes[0].get());
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
      emit_type_check(arg, "String", "parameter 'path'", argsAst.nodes[0].get());
      auto p = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto n = emit_call(module_->getFunction(rt::fs_size),
                         {p, line, col});
      emit_value_release(arg);
      return make_long(n);
    }

    // (path, line, col) -> Array*.
    if (method == "list_dir" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "parameter 'path'", argsAst.nodes[0].get());
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
      emit_type_check(arg, "String", "parameter 'path'", argsAst.nodes[0].get());
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
      emit_type_check(secs, "Float", "parameter 'secs'", a[0].get());
      auto d = jit.coerce_to_double(secs);
      emit_call(module_->getFunction(rt::time_sleep), {d});
      emit_value_release(secs);
      return make_nil();
    }
    if (method == "from_iso_nanos" && a.size() == 1) {
      auto arg = compile(*a[0]);
      emit_type_check(arg, "String", "parameter 's'", a[0].get());
      auto p = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto n = emit_call(module_->getFunction(rt::time_from_iso_nanos),
                         {p, line, col});
      emit_value_release(arg);
      return make_long(n);
    }
    if (method == "parse_nanos" && a.size() == 2) {
      auto s = compile(*a[0]);
      emit_type_check(s, "String", "parameter 's'", a[0].get());
      auto f = compile(*a[1]);
      emit_type_check(f, "String", "parameter 'fmt'", a[1].get());
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
      emit_type_check(f, "String", "parameter 'fmt'", a[1].get());
      auto fp = builder_.CreateIntToPtr(extract_data(f), ptrTy);
      auto u = eat_bool_i64(*a[2]);
      auto s = emit_call(module_->getFunction(rt::time_format_nanos),
                         {n, fp, u});
      emit_value_release(f);
      return make_string(s);
    }
    if (method == "from_parts_nanos" && a.size() == 2) {
      auto obj = compile(*a[0]);
      emit_type_check(obj, "Object", "parameter 'p'", a[0].get());
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
      emit_type_check(unit, "String", "parameter 'unit'", a[1].get());
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
      auto loV = compile(*argsAst.nodes[0]); emit_canon_arg_check(0, loV);
      auto hiV = compile(*argsAst.nodes[1]); emit_canon_arg_check(1, hiV);
      auto lo = value_to_long(loV);
      auto hi = value_to_long(hiV);
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
           extract_tag(b), extract_data(b), line, col});
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
      emit_type_check(arg, "Array", "parameter 'a'", argsAst.nodes[0].get());
      auto ap = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      emit_call(module_->getFunction(rt::random_shuffle), {ap});
      emit_value_release(arg);
      return make_nil();
    }
    if (method == "weighted_choice" && argsAst.nodes.size() == 2) {
      auto pop = compile(*argsAst.nodes[0]);
      emit_type_check(pop, "Array", "parameter 'pop'", argsAst.nodes[0].get());
      auto wts = compile(*argsAst.nodes[1]);
      emit_type_check(wts, "Array", "parameter 'weights'",
                      argsAst.nodes[1].get());
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
    // JSON.stringify / parse route through the shared canonical resolver (like
    // Http): NsParamMeta derived from the interp signature drives positional
    // binding (indent / lines as a 2nd positional), kwargs, **splats, arity,
    // and typed-positional checks, so every call shape stays byte-identical to
    // the interp. The runtime adapters (_ns_json_{stringify,parse}) consume the
    // resolved slab and self-validate each typed param for the as-value path.
    if (const NsMethod* m = _lookup_ns_method("JSON", method)) {
      return compile_ns_method_kwargs(jit, m, argsAst, callAst);
    }
  }

  if (ns == "Sys") {
    if (method == "exit" && argsAst.nodes.size() == 1) {
      auto codeVal = compile(*argsAst.nodes[0]);
      emit_type_check(codeVal, "Long", "parameter 'code'",
                      argsAst.nodes[0].get());
      auto code = value_to_long(codeVal);
      emit_call(module_->getFunction(rt::sys_exit), {code});
      return make_nil();  // unreachable; sys_exit terminates the process
    }
    if (method == "env" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "parameter 'name'", argsAst.nodes[0].get());
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
      // Match interp's generic param-type message ("parameter 'a' expects
      // Array"); Tensor.from's stdlib param is named `a`.
      emit_type_check(arg, "Array", "parameter 'a'", argsAst.nodes[0].get());
      auto ap = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto t = emit_call(
          module_->getFunction(rt::tensor_from), {ap, line, col});
      emit_value_release(arg);
      return make_tensor(t);
    }
    if (method == "from_csv" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "parameter 'path'", argsAst.nodes[0].get());
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

inline llvm::Value* JitExtension::emit_malformed_arg_throw(
    JIT& jit, const peg::Ast& argsAst, size_t eval_count, const char* kind,
    const std::string& msg, long line, long col) {
  CULEBRA_JIT_EXT_BODY_ALIASES(jit);
  using namespace peg::udl;
  for (size_t i = 0; i < eval_count && i < argsAst.nodes.size(); i++) {
    const auto& child = argsAst.nodes[i];
    const peg::Ast* expr = child->tag == "KWARG"_ ? child->nodes[1].get()
                         : child->tag == "KWARG_SPLAT"_ ? child->nodes[0].get()
                                                        : child.get();
    emit_value_release(compile(*expr));
  }
  jit.emit_throw_error(kind, msg, static_cast<size_t>(line),
                       static_cast<size_t>(col));
  return make_nil();  // unreachable: emit_throw_error is noreturn
}

inline llvm::Value* JitExtension::compile_single_positional_kwargs(
    JIT& jit, const peg::Ast& argsAst, const peg::Ast& callAst,
    const char* ctx, const char* rt_name, const NsParamMeta* meta) {
  CULEBRA_JIT_EXT_BODY_ALIASES(jit);
  using namespace peg::udl;
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto i64Ty = builder_.getInt64Ty();

  // Scan ARG_LIST: cmd + (kwargs / **splat). Extra positionals after cmd are
  // folded into the kwarg slab below, bound to the leading params by name.
  auto scanned = _jit_scan_arg_list(argsAst);
  auto& positional = scanned.positional;
  auto& explicit_kwargs = scanned.explicit_kwargs;
  auto& seen_explicit = scanned.seen_explicit;
  auto& splats = scanned.splats;
  // Structural syntax error (duplicate keyword / positional-after-keyword): the
  // scan recorded it instead of throwing, so re-emit it as a runtime throw at
  // its evaluate-then-error point — catchable like interp + the general path.
  if (scanned.err_kind) {
    return emit_malformed_arg_throw(jit, argsAst, scanned.err_eval_count,
                                    scanned.err_kind, scanned.err_msg,
                                    scanned.err_line, scanned.err_col);
  }
  // Arity (too few / too many positionals) and positional-vs-keyword conflict
  // are statically known here, but interp reports them at RUNTIME (catchable,
  // after evaluating every argument). Mirror that: evaluate all args, then emit
  // a runtime IR throw — so a `try` catches it on every backend, like interp.
  // (cmd is required; extra positionals bind the leading params cwd/env/… by
  // name.)
  bool too_few = positional.empty();
  bool too_many = positional.size() > static_cast<size_t>(meta->n_params);
  std::string conflict_param;  // first param given both positionally and by kw
  for (size_t i = 1; i < positional.size() &&
                     i < static_cast<size_t>(meta->n_params); i++) {
    if (seen_explicit.count(meta->params[i].name)) {
      conflict_param = std::string(meta->params[i].name);
      break;
    }
  }
  if (too_few || too_many || !conflict_param.empty()) {
    std::string msg =
        (too_few || too_many)
            ? culebra::ns_fn_arity_error_message(
                  too_few ? meta->min_arity : meta->n_params,
                  static_cast<long>(positional.size()))
            : std::format(
                  "got argument '{}' both positionally and as a keyword",
                  conflict_param);
    return emit_malformed_arg_throw(
        jit, argsAst, argsAst.nodes.size(),
        (too_few || too_many) ? "ArityError" : "TypeError", msg,
        callAst.line, callAst.column);
  }

  auto cmd_val = compile(*positional[0]);
  // The positional command list is a typed `Array` param on the interp side
  // (`cmd` for run/spawn, `commands` for all); mirror its message + argument
  // position here so a non-Array is rejected identically (the runtime impls
  // keep a backstop check for the element/empty cases).
  jit.emit_type_check(cmd_val, "Array",
                      std::strcmp(ctx, "Proc.all") == 0 ? "parameter 'commands'"
                                                        : "parameter 'cmd'",
                      positional[0]);
  std::vector<llvm::Constant*> kwKeys;
  std::vector<llvm::Value*> kwVals;
  for (auto& [name, ast] : explicit_kwargs) {
    kwKeys.push_back(builder_.CreateGlobalString(std::string(name), ".kwkey"));
    kwVals.push_back(compile(*ast));
  }
  // Positionals after cmd → kwargs named after params[1..] (cwd, env, …). A
  // param given both positionally and by keyword is an error, matching interp.
  for (size_t i = 1; i < positional.size(); i++) {
    std::string_view pname = meta->params[i].name;  // conflict already ruled out
    kwKeys.push_back(builder_.CreateGlobalString(std::string(pname), ".kwkey"));
    auto pv = compile(*positional[i]);
    // A typed param bound positionally (e.g. Proc.run's cwd: String?) is checked
    // at the argument's position, matching the interp binder — otherwise the
    // adapter rejects it later at the call site. No-op for untyped params.
    jit.emit_type_check(pv, meta->params[i].type,
                        std::format("parameter '{}'", pname), positional[i]);
    kwVals.push_back(pv);
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

inline llvm::Value* JitExtension::compile_ns_method_kwargs(
    JIT& jit, const NsMethod* m, const peg::Ast& argsAst,
    const peg::Ast& callAst) {
  CULEBRA_JIT_EXT_BODY_ALIASES(jit);
  using namespace peg::udl;
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto i64Ty = builder_.getInt64Ty();

  // Scan ARG_LIST into positionals / explicit kwargs / splats (same shape as
  // compile_single_positional_kwargs, but N positionals).
  auto scanned = _jit_scan_arg_list(argsAst);
  // Structural syntax error (duplicate keyword / positional-after-keyword):
  // re-emit as a runtime throw, catchable like interp (the scan no longer
  // throws at compile time).
  if (scanned.err_kind) {
    return emit_malformed_arg_throw(jit, argsAst, scanned.err_eval_count,
                                    scanned.err_kind, scanned.err_msg,
                                    scanned.err_line, scanned.err_col);
  }
  auto& positional = scanned.positional;
  auto& explicit_kwargs = scanned.explicit_kwargs;
  auto& splats = scanned.splats;

  // Compile each positional once (no type check yet). interp evaluates ALL
  // arguments first, then checks arity, then type-checks per param — so a too-
  // many/too-few call raises ArityError before any positional type error. We
  // therefore defer the type checks until after the arity gate below.
  std::vector<llvm::Value*> posVals;
  posVals.reserve(positional.size());
  const NsParamMeta* pm = _ns_meta(m);
  for (size_t i = 0; i < positional.size(); i++) {
    posVals.push_back(compile(*positional[i]));
  }

  std::vector<llvm::Constant*> kwKeys;
  std::vector<llvm::Value*> kwVals;
  for (auto& [name, ast] : explicit_kwargs) {
    kwKeys.push_back(builder_.CreateGlobalString(std::string(name), ".kwkey"));
    kwVals.push_back(compile(*ast));
  }
  std::vector<llvm::Value*> splatVals;
  for (auto* ast : splats) splatVals.push_back(compile(*ast));

  // Arity gate, after all arguments are evaluated — interp's strict_arity block
  // fires here, before the per-param type checks and before splat validation.
  // (Non-pm codecs keep the resolver's own checks.) args-rest methods carry no
  // positional cap.
  if (pm && pm->args_rest_idx < 0) {
    long npos = static_cast<long>(positional.size());
    if (npos < pm->min_arity || npos > pm->max_arity) {
      jit.emit_throw_error(
          "ArityError",
          culebra::ns_fn_arity_error_message(
              npos < pm->min_arity ? pm->min_arity : pm->max_arity, npos),
          static_cast<size_t>(callAst.line),
          static_cast<size_t>(callAst.column));
      return make_nil();  // unreachable: emit_throw_error is noreturn
    }
  }

  // Now the per-param type checks, at each argument's source position (matching
  // interp's binder). A param-less codec carries a single arg0_type (param "s").
  for (size_t i = 0; i < positional.size(); i++) {
    std::string_view ptype;
    std::string pname;
    if (pm) {
      if (static_cast<int>(i) < pm->n_params) {
        ptype = pm->params[i].type;
        pname = std::string(pm->params[i].name);
      }
    } else if (i == 0) {
      if (m->arg0_type) ptype = m->arg0_type;
      if (m->arg0_name) pname = m->arg0_name;
    }
    if (!ptype.empty()) {
      jit.emit_type_check(posVals[i], ptype, std::format("parameter '{}'", pname),
                          positional[i]);
    }
  }

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
  auto posSlab = alloc_slab(jit.valueType_, posVals.size(), "ns.pos");
  auto keysSlab = alloc_slab(ptrTy, kwKeys.size(), "ns.kw.keys");
  auto valsSlab = alloc_slab(jit.valueType_, kwVals.size(), "ns.kw.vals");
  auto splatSlab = alloc_slab(jit.valueType_, splatVals.size(), "ns.kw.splat");
  for (size_t i = 0; i < posVals.size(); i++)
    store_at(posSlab, jit.valueType_, i, posVals[i]);
  for (size_t i = 0; i < kwKeys.size(); i++) {
    store_at(keysSlab, ptrTy, i, kwKeys[i]);
    store_at(valsSlab, jit.valueType_, i, kwVals[i]);
  }
  for (size_t i = 0; i < splatVals.size(); i++)
    store_at(splatSlab, jit.valueType_, i, splatVals[i]);

  // Identify the method by name (AOT-safe; a baked NsMethod* would be invalid
  // in the separately-linked AOT binary). `sub` is null for top-level methods.
  auto nsStr = jit.get_or_create_global_str(m->ns, ".nsm.ns");
  auto methodStr = jit.get_or_create_global_str(m->name, ".nsm.method");
  auto subStr = m->sub ? jit.get_or_create_global_str(m->sub, ".nsm.sub")
                       : llvm::ConstantPointerNull::get(ptrTy);
  // Signature: (ns, method, sub, n_pos, pos*, n_kw, keys*, vals*, n_splat,
  // splat*, line, col) -> JitValue. The entry consumes all arg values.
  return emit_call(
      module_->getOrInsertFunction(
          rt::ns_method_call_kw, jit.valueType_, ptrTy, ptrTy, ptrTy, i64Ty,
          ptrTy, i64Ty, ptrTy, ptrTy, i64Ty, ptrTy, i64Ty, i64Ty),
      {nsStr, methodStr, subStr,
       builder_.getInt64(static_cast<int64_t>(posVals.size())), posSlab,
       builder_.getInt64(static_cast<int64_t>(kwVals.size())), keysSlab,
       valsSlab, builder_.getInt64(static_cast<int64_t>(splatVals.size())),
       splatSlab, builder_.getInt64(callAst.line),
       builder_.getInt64(callAst.column)});
}

inline llvm::Value* JitExtension::compile_nested_ns_call(
    JIT& jit, std::string_view ns, std::string_view sub,
    std::string_view method, const peg::Ast& argsAst,
    const peg::Ast& callAst) {
  // (Shadowing — `let Encoding = {...}` — is handled uniformly upstream in
  // compile_call_with_builtins, which routes a shadowed callee to compile_call
  // before any builtin dispatch.)
  // Only the statically-known nested codecs (e.g. `Encoding.base64.encode`),
  // and only positional-only calls — anything else (an unknown method, kwargs
  // on a param-less codec) falls through to the generic sub-namespace-object
  // method dispatch, which matches interp there.
  if (!JIT::arg_list_is_positional_only(argsAst)) return nullptr;
  std::string sub_s(sub);
  const NsMethod* m = _lookup_ns_method(ns, method, sub_s.c_str());
  if (!m) return nullptr;
  return compile_ns_method_kwargs(jit, m, argsAst, callAst);
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
      "_Regex",  "Proc",      "Isolate",   "Channel",  "Parallel",
      "Signal",  "Encoding", "Compress",  "SharedBuffer", "Shared",
      "Hash",
#if defined(CULEBRA_HTTP_ENABLED)
      "Http",
#endif
  };
  if (names.contains(name)) return true;
  // wrap.h-declared namespaces (e.g. __Foreign) — registry-driven.
  for (const auto& m : _wrapped_ns_methods()) {
    if (name == m.ns) return true;
  }
  return false;
}

inline llvm::Value* JitExtension::compile_ufcs_builtin(
    JIT& jit, const std::string& method, const peg::Ast& argsAst,
    llvm::Value* receiver) {
  using namespace peg::udl;
  CULEBRA_JIT_EXT_BODY_ALIASES(jit);
  auto line = jit.current_line_val();
  auto col = jit.current_column_val();
  auto ptrTy = llvm::PointerType::get(ctx_, 0);

  // `range`/`iota` are sequence-factory globals, so UFCS feeds the
  // receiver in as their first positional argument: `(3).range()` →
  // range(3) = 0..3, `(0).range(5)` → range(0, 5). value_to_long on a
  // non-Long receiver raises "expected Long, got <T>", matching interp
  // (so the nonsensical `Math.range(3)` lands on the same error). `range`
  // also accepts a `step:` kwarg (`(0).range(10, step: 2)`), mirroring the
  // bare-call path; everything else (splat / unknown kwarg / 3+ start-end
  // positionals) falls through to regular dispatch.
  if (method == "range" || method == "iota") {
    std::vector<const peg::Ast*> positional;
    const peg::Ast* step_ast = nullptr;
    bool bail = false;
    bool unknown_kw = false;
    std::string unknown_kw_name;
    const peg::Ast* unknown_kw_node = nullptr;
    for (auto& c : argsAst.nodes) {
      if (c->tag == "KWARG_SPLAT"_) { bail = true; break; }  // splat: deferred
      if (c->tag == "KWARG"_) {
        auto kwname = std::string(c->nodes[0]->token);
        if (method == "range" && kwname == "step") {
          step_ast = c->nodes[1].get();
        } else {
          // range/iota take only `step:` (range) — any other keyword is
          // unknown, matching interp's runtime TypeError.
          unknown_kw = true;
          unknown_kw_name = kwname;
          unknown_kw_node = c.get();
          break;
        }
      } else {
        positional.push_back(c.get());
      }
    }
    if (unknown_kw) {
      jit.emit_throw_error(
          "TypeError",
          std::format("unknown keyword argument '{}'", unknown_kw_name),
          unknown_kw_node->line, unknown_kw_node->column);
      return jit.make_nil();  // unreachable after the throw
    }
    // The receiver is the implicit first positional, so the total is
    // 1 (receiver only) or 2 (receiver + start/end). More is an
    // ArityError, matching interp + the bare-call path.
    if (!bail) {
      long total = 1 + static_cast<long>(positional.size());
      if (total > 2) {
        jit.emit_throw_error("ArityError",
                             culebra::builtin_arity_error_message(method, 1, 2,
                                                                  total),
                             argsAst.line, argsAst.column);
        return jit.make_nil();  // unreachable after the throw
      }
      llvm::Value* s;
      llvm::Value* e;
      if (positional.empty()) {  // (N).range() → range(0, N)
        s = jit.builder_.getInt64(0);
        e = jit.value_to_long(receiver);
      } else {  // (S).range(E) → range(S, E)
        s = jit.value_to_long(receiver);
        e = jit.value_to_long(jit.compile(*positional[0]));
      }
      if (method == "iota") {
        auto arr = emit_call(module_->getFunction(rt::iota), {s, e});
        return jit.make_array(arr);
      }
      auto step = step_ast ? jit.value_to_long(jit.compile(*step_ast))
                           : jit.builder_.getInt64(1);
      auto obj = emit_call(module_->getFunction(rt::math_range),
                           {s, e, step, line, col});
      return jit.make_object(obj);
    }
  }

  // Arity-1 global builtins reachable via UFCS (`x.f()` == `f(x)`). interp
  // resolves these against the global env, so they all work as methods and an
  // extra positional arg is an ArityError on the arity-1 global — not the
  // property-get TypeError the JIT used to fall into when it only handled the
  // 0-arg form. (hash and to_float were also missing here entirely, so
  // `x.hash()` / `x.to_float()` failed outright.) puts/print are variadic and
  // handled separately below.
  static const std::set<std::string_view> arity1_globals = {
      "to_long", "to_float", "to_string", "type_of", "hash"};
  if (arity1_globals.contains(method) && argsAst.nodes.size() != 0) {
    long extra = static_cast<long>(argsAst.nodes.size());
    // `to_string` is *also* a 0-arg value-method on String/StringView in the
    // interp (the StringView→String materializer), which reports a
    // value-method arity message for those receivers; every other type uses
    // the arity-1 global. Branch on the receiver tag so the JIT matches interp
    // byte-for-byte. (The receiver type isn't known until runtime.)
    if (method == "to_string") {
      auto fn = builder_.GetInsertBlock()->getParent();
      auto svBB = llvm::BasicBlock::Create(ctx_, "tostr.arity.sv", fn);
      auto glBB = llvm::BasicBlock::Create(ctx_, "tostr.arity.gl", fn);
      auto deadBB = llvm::BasicBlock::Create(ctx_, "tostr.arity.dead", fn);
      auto tag = extract_tag(receiver);
      auto isStr = builder_.CreateOr(
          builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_STRING)),
          builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_STRINGVIEW)),
          "tostr.is.str");
      builder_.CreateCondBr(isStr, svBB, glBB);
      // String/StringView: value-method (0 expected, `extra` given).
      builder_.SetInsertPoint(svBB);
      jit.emit_throw_error(
          "ArityError",
          culebra::builtin_arity_error_message("to_string", 0, 0, extra),
          argsAst.line, argsAst.column);
      builder_.CreateBr(deadBB);
      // Everything else: arity-1 global (receiver is arg 0, so 1 + extra).
      builder_.SetInsertPoint(glBB);
      jit.emit_throw_error("ArityError",
                           culebra::ns_fn_arity_error_message(1, 1 + extra),
                           argsAst.line, argsAst.column);
      builder_.CreateBr(deadBB);
      builder_.SetInsertPoint(deadBB);
      return make_nil();  // unreachable: both predecessors throw
    }
    // Other arity-1 globals have no value-method form: the receiver is the
    // implicit arg 0, so the total is 1 + the extras. The nameless message
    // matches interp's global-UFCS arity error.
    jit.emit_throw_error("ArityError",
                         culebra::ns_fn_arity_error_message(1, 1 + extra),
                         argsAst.line, argsAst.column);
    return make_nil();  // unreachable after the throw
  }

  if (argsAst.nodes.size() != 0) return nullptr;
  if (method == "puts" || method == "print") {
    auto rt_name = method == "puts" ? rt::puts : rt::print;
    emit_call(module_->getFunction(rt_name),
                        {extract_tag(receiver), extract_data(receiver)});
    emit_value_release(receiver);
    return make_nil();
  }
  // to_long/to_float attribute a bad-conversion error to the call's argument
  // list, matching interp's UFCS path (eval_ufcs_call invokes with
  // args_ast.line/column, which the builtin reads as __LINE__/__COLUMN__).
  // The generic `line`/`col` above point at the receiver, so use the args
  // node here.
  auto argLine = builder_.getInt64(argsAst.line);
  auto argCol = builder_.getInt64(argsAst.column);
  if (method == "to_long") {
    // Polymorphic Long/Float/String, mirroring compile_global's bare
    // `to_long(x)` — interp's `(123).to_long()` accepts a Long, so the
    // String-only `rt::to_long` here used to wrongly reject it.
    auto r = emit_call(
        module_->getFunction(rt::to_long_any),
        {extract_tag(receiver), extract_data(receiver), argLine, argCol});
    emit_value_release(receiver);
    return r;
  }
  if (method == "to_float") {
    // Polymorphic Long/Float/String, mirroring compile_global's bare
    // `to_float(x)`. Returns a full Value (the runtime picks the tag).
    auto r = emit_call(
        module_->getFunction(rt::to_float_any),
        {extract_tag(receiver), extract_data(receiver), argLine, argCol});
    emit_value_release(receiver);
    return r;
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
  if (method == "hash") {
    auto h = emit_call(
        module_->getFunction(rt::hash_any),
        {extract_tag(receiver), extract_data(receiver), line, col});
    emit_value_release(receiver);
    return make_long(h);
  }
  return nullptr;
}

#undef CULEBRA_JIT_EXT_BODY_ALIASES

}  // namespace culebra
