#pragma once

// The compiled lanes' implementation of the Culebra standard library.
//
// Independent header. Include from main.cc (or any embedder). Provides the
// runtime functions compiled code calls and the `JitExtension` struct that
// fills `culebra::ExtensionHooks`. Embedders install the stdlib by calling
// `culebra::install_jit_stdlib()` once before running a program.
//
// Only one member needs LLVM — `declare_runtime`, which declares those
// functions on the module being built — so the body of this header sits on
// rt.h and a build without the JIT gets the whole stdlib anyway.

#include <compress.h>
#include <csv.h>
#include <env.h>
#include <hash.h>
#include <json.h>
#include <toml.h>
#include <uuid.h>
#include <vfs.h>
#include <rt.h>
#include <net.h>
#include <proc.h>
#include <canvas.h>
#include <font_ttf.h>
#include <image.h>
#include <term.h>
#if defined(CULEBRA_HTTP_ENABLED)
#include <http.h>
#endif
#if defined(CULEBRA_SQLITE_ENABLED)
#include <sqlite.h>
#endif
#include <canon_sigs.h>  // the canonical native/built-in signature tables
#include <fswatcher.h>   // FS.watch core (fswatch::)
#include <wrap_registry.h>  // the wrap.h compiled-lane rows
#include <stdlib_kernels.h>  // File/FS/Time/Net/Regex kernels shared with interp
#include <shared.h>
#include <regexlib.h>
#include <sendable_jit.h>  // JIT isolate transfer (jit_serialize, spawn, handle)
#include <stdlib_math.h>   // Math kernels shared with the interp
#ifdef CULEBRA_JIT_ENABLED
#include <jit.h>  // JitExtension::declare_runtime emits on the module
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>  // std::numbers::pi / ::e (Math.pi / Math.e; portable vs M_PI)
#include <os_compat.h>  // os_setenv / os_strptime (setenv / strptime shims)
#include <system_error>
#include <unistd.h>  // isatty (IO.*_is_terminal)

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
    int8_t tag, int64_t data, int64_t line, int64_t col, int64_t base) {
  bool str = tag == TAG_STRING || tag == TAG_STRINGVIEW;
  // A base only means something for a string to parse (see the interp's
  // to_long); naming one for a number is a mistake, not an ignored arg.
  if (!str && base != 10) {
    throw culebra::CulebraError("TypeError",
                                "to_long() base is only valid for a String",
                                line, col);
  }
  if (tag == TAG_LONG) return {TAG_LONG, data};
  if (tag == TAG_FLOAT) {
    return {TAG_LONG, static_cast<int64_t>(_culebra_float_to_double(data))};
  }
  if (str) {
    auto sv = _culebra_str_view(tag, data);
    return {TAG_LONG,
            parse_long_strict(std::string(sv).c_str(), line, col, base)};
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

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_println(int8_t type,
                                                        int64_t data) {
  culebra_runtime_print(type, data);
  std::cout << std::endl;
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

// The content arrives as (tag, data), not a bare `const char*`: a culebra
// String is a BYTE string, so its length has to come from the string's own
// header rather than strlen — which stopped at the first NUL and truncated
// every binary write (a PNG, a gzip blob) to its first few bytes under the
// JIT while the interp wrote it whole.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_write_file(
    const char* path, int8_t tag, int64_t data, int64_t line, int64_t col) {
  std::ofstream ofs(path, std::ios::binary);
  if (!ofs) {
    throw culebra::CulebraError("IOError",
        std::format("FS.write: cannot open '{}'", path), line, col);
  }
  auto sv = _culebra_str_view(tag, data);
  ofs.write(sv.data(), static_cast<std::streamsize>(sv.size()));
}

// The Math bodies below delegate to the kernels in stdlib_math.h — the
// same ones the interp's make_math_namespace calls — so the semantic
// decisions (Long/Float promotion, exact Long comparison, edge-case
// errors) exist once across backends. These shims only translate the JIT
// value representation to and from culebra::math::Num.
inline culebra::math::Num _math_num(int8_t tag, int64_t data, int64_t line,
                                    int64_t col) {
  if (tag == TAG_LONG) return culebra::math::num_long(data);
  if (tag == TAG_FLOAT)
    return culebra::math::num_float(_culebra_float_to_double(data));
  throw_type_error_at(line, col);
}
inline JitValue _math_out(const culebra::math::Num& n) {
  return n.is_float ? JitValue{TAG_FLOAT, _culebra_double_to_bits(n.d)}
                    : JitValue{TAG_LONG, n.l};
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_math_pow(
    int64_t base, int64_t exp, int64_t line, int64_t col) {
  return culebra::math::pow_long(base, exp,
                                 [&] { return std::pair(line, col); });
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_math_wrap(
    int64_t x, int64_t n, int64_t line, int64_t col) {
  return culebra::math::wrap(x, n, [&] { return std::pair(line, col); });
}

#define CUL_MATH_F2F(name, fn)                                          \
  CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_##name(    \
      int8_t tag, int64_t data, int64_t line, int64_t col) {            \
    return {TAG_FLOAT,                                                  \
            _culebra_double_to_bits(culebra::math::f2f(                 \
                _math_num(tag, data, line, col),                        \
                [](double x) { return fn(x); }))};                      \
  }
CUL_MATH_F2F(log,  std::log)
CUL_MATH_F2F(exp,  std::exp)
CUL_MATH_F2F(sqrt, std::sqrt)
CUL_MATH_F2F(sin,  std::sin)
CUL_MATH_F2F(cos,  std::cos)
CUL_MATH_F2F(tan,  std::tan)
CUL_MATH_F2F(asin, std::asin)
CUL_MATH_F2F(acos, std::acos)
CUL_MATH_F2F(atan, std::atan)
#undef CUL_MATH_F2F

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_atan2(
    int8_t yt, int64_t yd, int8_t xt, int64_t xd, int64_t line, int64_t col) {
  return {TAG_FLOAT, _culebra_double_to_bits(culebra::math::atan2(
                         _math_num(yt, yd, line, col),
                         _math_num(xt, xd, line, col)))};
}

#define CUL_MATH_F2L(name, fn)                                          \
  CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_##name(    \
      int8_t tag, int64_t data, int64_t line, int64_t col) {            \
    return _math_out(culebra::math::f2l(                                \
        _math_num(tag, data, line, col),                                \
        [](double x) { return fn(x); }));                               \
  }
CUL_MATH_F2L(floor, std::floor)
CUL_MATH_F2L(ceil,  std::ceil)
CUL_MATH_F2L(round, std::rint)
#undef CUL_MATH_F2L

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_abs(
    int8_t tag, int64_t data, int64_t line, int64_t col) {
  return _math_out(culebra::math::abs(_math_num(tag, data, line, col)));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_min(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  return _math_out(culebra::math::reduce_min_max(
      n, /*pick_less=*/true,
      [&](int64_t i) { return _math_num(args[i].tag, args[i].data, line, col); },
      [&] { return std::pair(line, col); }));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_max(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  return _math_out(culebra::math::reduce_min_max(
      n, /*pick_less=*/false,
      [&](int64_t i) { return _math_num(args[i].tag, args[i].data, line, col); },
      [&] { return std::pair(line, col); }));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_math_clamp(
    int8_t xt, int64_t xd, int8_t lot, int64_t lod, int8_t hit, int64_t hid,
    int64_t line, int64_t col) {
  return _math_out(culebra::math::clamp(_math_num(xt, xd, line, col),
                                        _math_num(lot, lod, line, col),
                                        _math_num(hit, hid, line, col)));
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

// choice(pop): returns one uniformly-random element from pop. Returned
// value is +1 owned by the caller (we retain the picked element before
// returning).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_random_choice(
    JitArray* pop, int64_t line, int64_t col) {
  if (!pop || pop->size == 0) {
    throw_type_error_at(line, col);
  }
  std::uniform_int_distribution<size_t> d(0, pop->size - 1);
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
    // PathLike per component: String as-is, Path via __str__ (symmetric with
    // the interp `_fspath` and the adapter `take_path`).
    if (args[i].tag == TAG_STRING) {
      out /= reinterpret_cast<const char*>(args[i].data);
    } else if (args[i].tag == TAG_STRINGVIEW ||
               _culebra_value_matches_type(args[i].tag, args[i].data, "Path")) {
      out /= culebra_runtime_value_to_display(args[i].tag, args[i].data);
    } else {
      culebra::throw_runtime_error_at(
          "TypeError",
          culebra::type_mismatch_message("String|Path",
                                         culebra_runtime_type_of(args[i].tag)),
          line, col);
    }
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

// `FS.stat(path)` -> Object{size, is_dir, is_file, is_symlink, mtime, mode,
// uid, gid}.
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
  int64_t mode = static_cast<int64_t>(
      fst.permissions() & std::filesystem::perms::mask);
  culebra_runtime_object_set(o, "mode", false, TAG_LONG, mode, line, col);
  int64_t uid = -1, gid = -1;
  culebra::_fs_owner(path ? path : "", uid, gid);
  culebra_runtime_object_set(o, "uid", false, TAG_LONG, uid, line, col);
  culebra_runtime_object_set(o, "gid", false, TAG_LONG, gid, line, col);
  return o;
}

// `FS.chmod(path, mode)` — set permission bits, e.g. 0o755. `mode` is masked
// to the low 12 bits (rwx + setuid/setgid/sticky).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_fs_chmod(
    const char* path, int64_t mode, int64_t line, int64_t col) {
  std::error_code ec;
  std::filesystem::permissions(
      path ? path : "",
      static_cast<std::filesystem::perms>(
          mode & static_cast<int64_t>(std::filesystem::perms::mask)),
      std::filesystem::perm_options::replace, ec);
  if (ec) _fs_throw_io(
      std::format("FS.chmod('{}')", path ? path : ""), line, col, ec);
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
// in stdlib_interp.h) — interp registers it lazily, JIT/AOT register its
// builder ahead of every module via `splice_stdlib_preamble`.
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

// --- _Term primitives ---
// Terminal-control thin wrappers (logic in term.h, shared with interp).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_term_cols() {
  return culebra::_term_detail::cols();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_term_rows() {
  return culebra::_term_detail::rows();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_term_raw_on() {
  culebra::_term_detail::raw_on();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_term_raw_off() {
  culebra::_term_detail::raw_off();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_term_flush() {
  culebra::_term_detail::flush();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_term_resized() {
  return culebra::_term_detail::take_resize();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_term_width(
    const char* s) {
  return culebra::_term_detail::width(s ? s : "");
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_term_color_level() {
  return culebra::_term_detail::color_level();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_term_read_key(
    double timeout) {
  return _culebra_heap_str(culebra::_term_detail::read_key(timeout));
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_term_attach_tty() {
  return culebra::_term_detail::attach_tty();
}

// --- _Canvas primitives ---
// Immediate-mode 2D framebuffer thin wrappers (logic in canvas.h, shared with
// interp). All positional. sprite_load reads its pixels straight off the
// JitArray runtime struct (packed-RGBA Longs), mirroring tensor_from.
// A Float geometry argument goes through canvas_coord — the same canvas.h
// rule interp uses — rather than an inlined fptosi, so rounding can't drift
// between backends.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_coord(
    double d) {
  return culebra::_canvas_detail::coord(d);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_init(
    int64_t w, int64_t h, int64_t line, int64_t col) {
  if (!culebra::_canvas_detail::init(static_cast<int>(w), static_cast<int>(h)))
    throw culebra::CulebraError("RuntimeError",
                                culebra::_canvas_detail::kBusyError, line, col);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_clear(
    int64_t rgba) {
  culebra::_canvas_detail::clear(static_cast<uint32_t>(rgba));
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_set_pixel(
    int64_t x, int64_t y, int64_t rgba) {
  culebra::_canvas_detail::set_pixel(static_cast<int>(x), static_cast<int>(y),
                                     static_cast<uint32_t>(rgba));
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_get_pixel(
    int64_t x, int64_t y) {
  return static_cast<int64_t>(
      culebra::_canvas_detail::get_pixel(static_cast<int>(x),
                                         static_cast<int>(y)));
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_line(
    int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t rgba) {
  culebra::_canvas_detail::line(x1, y1, x2, y2,
                                static_cast<uint32_t>(rgba));
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_ellipse(
    int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t rgba,
    int64_t fill) {
  culebra::_canvas_detail::ellipse(cx, cy, rx, ry,
                                   static_cast<uint32_t>(rgba), fill != 0);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_rect(
    int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgba, int64_t fill) {
  culebra::_canvas_detail::rect(static_cast<int>(x), static_cast<int>(y),
                                static_cast<int>(w), static_cast<int>(h),
                                static_cast<uint32_t>(rgba), fill != 0);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_triangle(
    int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t x3, int64_t y3,
    int64_t rgba, int64_t fill) {
  culebra::_canvas_detail::triangle(x1, y1, x2, y2, x3, y3,
                                    static_cast<uint32_t>(rgba), fill != 0);
}
// The `points` elements carry the same Long|Float contract the scalar geometry
// arguments do, so the tags are checked here rather than narrowed blindly.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_polygon(
    JitArray* points, int64_t rgba, int64_t fill, int64_t line, int64_t col) {
  std::vector<int64_t> pts;
  if (points) {
    pts.reserve(points->size);
    for (size_t i = 0; i < points->size; i++) {
      const auto& e = points->items[i];
      if (e.tag == TAG_LONG) {
        pts.push_back(e.data);
      } else if (e.tag == TAG_FLOAT) {
        double d;
        std::memcpy(&d, &e.data, sizeof(d));
        pts.push_back(culebra::_canvas_detail::coord(d));
      } else {
        throw culebra::CulebraError(
            "TypeError", culebra::_canvas_detail::kPolygonPointsError, line,
            col);
      }
    }
  }
  culebra::_canvas_detail::polygon(pts.data(),
                                   static_cast<int64_t>(pts.size() / 2),
                                   static_cast<uint32_t>(rgba), fill != 0);
}
// A JitArray of Longs as a packed integer vector (null or empty -> empty), the
// upload-once shape both Canvas table prims take. The shims around it are the
// C-linkage runtime ABI; a template can't join that, so it names its own.
extern "C++" template <typename T>
inline std::vector<T> _jit_canvas_int_array(JitArray* a) {
  std::vector<T> out;
  if (!a) return out;
  out.reserve(a->size);
  for (size_t i = 0; i < a->size; i++)
    out.push_back(static_cast<T>(a->items[i].data));
  return out;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_font_load(
    JitArray* rows) {
  auto bytes = _jit_canvas_int_array<uint8_t>(rows);
  return culebra::_canvas_detail::font_load(
      bytes.data(), static_cast<int64_t>(bytes.size()));
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_glyph(
    int64_t font, int64_t index, int64_t x, int64_t y, int64_t rgba,
    int64_t scale) {
  culebra::_canvas_detail::glyph(font, index, static_cast<int>(x),
                                 static_cast<int>(y),
                                 static_cast<uint32_t>(rgba), scale);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_sprite_load(
    JitArray* pixels, int64_t w, int64_t h) {
  auto px = _jit_canvas_int_array<uint32_t>(pixels);
  return culebra::_canvas_detail::sprite_load(
      px.data(), static_cast<int64_t>(px.size()), static_cast<int>(w),
      static_cast<int>(h));
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t
culebra_runtime_canvas_sprite_from_png(uint8_t tag, int64_t data, int64_t line,
                                       int64_t col) {
  auto d = culebra::image::decode_png(_culebra_str_view(tag, data));
  if (!d.error.empty())
    throw culebra::CulebraError("ValueError", d.error, line, col);
  return culebra::_canvas_detail::sprite_adopt(std::move(d.px), d.w, d.h);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char*
culebra_runtime_canvas_sprite_to_png(
    int64_t id, int64_t line, int64_t col) {
  auto t = culebra::_canvas_detail::readback_target(id);
  if (t.px == nullptr) {
    auto r = culebra::_canvas_detail::refusal();
    throw culebra::CulebraError(r.kind, r.message, line, col);
  }
  auto e = culebra::image::encode_png(t.px, t.w, t.h);
  if (!e.error.empty())
    throw culebra::CulebraError("ValueError", e.error, line, col);
  return _culebra_heap_str(e.data);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_sprite_blank(
    int64_t w, int64_t h, int64_t rgba) {
  return culebra::_canvas_detail::sprite_blank(static_cast<int>(w),
                                               static_cast<int>(h),
                                               static_cast<uint32_t>(rgba));
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_sprite_free(
    int64_t id, int64_t line, int64_t col) {
  if (!culebra::_canvas_detail::sprite_free(id))
    throw culebra::CulebraError(
        "ValueError", culebra::_canvas_detail::kFreeTargetError, line, col);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_ttf_load(
    uint8_t tag, int64_t data, int64_t line, int64_t col) {
  auto r = culebra::_canvas_detail::ttf_load(_culebra_str_view(tag, data));
  if (!r.error.empty())
    throw culebra::CulebraError("ValueError", r.error, line, col);
  return r.id;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_ttf_free(
    int64_t id) {
  culebra::_canvas_detail::ttf_free(id);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_ttf_glyph(
    int64_t font, int64_t codepoint, int64_t x, int64_t y, int64_t rgba,
    int64_t size) {
  return culebra::_canvas_detail::ttf_glyph(
      font, codepoint, static_cast<int>(x), static_cast<int>(y),
      static_cast<uint32_t>(rgba), size);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t
culebra_runtime_canvas_ttf_glyph_screen(int64_t font, int64_t codepoint,
                                        int64_t x, int64_t y, int64_t rgba,
                                        int64_t size) {
  return culebra::_canvas_detail::ttf_glyph_screen(
      font, codepoint, static_cast<int>(x), static_cast<int>(y),
      static_cast<uint32_t>(rgba), size);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_screen_width() {
  return culebra::_canvas_detail::screen_width();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t
culebra_runtime_canvas_screen_height() {
  return culebra::_canvas_detail::screen_height();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t
culebra_runtime_canvas_get_screen_pixel(int64_t x, int64_t y) {
  return culebra::_canvas_detail::get_screen_pixel(static_cast<int>(x),
                                                   static_cast<int>(y));
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE double
culebra_runtime_canvas_screen_scale() {
  return culebra::_canvas_detail::screen_scale();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_ttf_advance(
    int64_t font, int64_t codepoint, int64_t size) {
  return culebra::_canvas_detail::ttf_advance(font, codepoint, size);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_ttf_ascent(
    int64_t font, int64_t size) {
  return culebra::_canvas_detail::ttf_ascent(font, size);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_target(
    int64_t id, int64_t line, int64_t col) {
  int64_t prev = culebra::_canvas_detail::target(id);
  if (prev < 0) {
    auto r = culebra::_canvas_detail::refusal();
    throw culebra::CulebraError(r.kind, r.message, line, col);
  }
  return prev;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_sprite_width(
    int64_t id) {
  return culebra::_canvas_detail::sprite_width(id);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_sprite_height(
    int64_t id) {
  return culebra::_canvas_detail::sprite_height(id);
}
inline void _culebra_canvas_blit_check(
    bool ok, int64_t line, int64_t col) {
  if (!ok)
    throw culebra::CulebraError(
        "ValueError", culebra::_canvas_detail::kSelfBlitError, line, col);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_blit(
    int64_t id, int64_t dx, int64_t dy, int64_t sx, int64_t sy, int64_t sw,
    int64_t sh, int64_t flags, int64_t line, int64_t col) {
  _culebra_canvas_blit_check(
      culebra::_canvas_detail::blit(id, static_cast<int>(dx),
                                    static_cast<int>(dy), static_cast<int>(sx),
                                    static_cast<int>(sy), static_cast<int>(sw),
                                    static_cast<int>(sh), flags),
      line, col);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_blit_scaled(
    int64_t id, int64_t dx, int64_t dy, int64_t dw, int64_t dh, int64_t sx,
    int64_t sy, int64_t sw, int64_t sh, int64_t flags, int64_t alpha,
    int64_t line, int64_t col) {
  _culebra_canvas_blit_check(
      culebra::_canvas_detail::blit_scaled(id, dx, dy, dw, dh, sx, sy, sw, sh,
                                           flags, alpha),
      line, col);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_present(
    int64_t line, int64_t col) {
  culebra::_canvas_detail::present();
  if (const char* e = culebra::_canvas_detail::window_error())
    throw culebra::CulebraError("RuntimeError", e, line, col);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_buttons() {
  return culebra::_canvas_detail::buttons();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_mouse_x() {
  return culebra::_canvas_detail::mouse_x();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_mouse_y() {
  return culebra::_canvas_detail::mouse_y();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t
culebra_runtime_canvas_mouse_buttons() {
  return culebra::_canvas_detail::mouse_buttons();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_canvas_key(uint8_t tag,
                                                                  int64_t data) {
  auto sv = _culebra_str_view(tag, data);
  return culebra::_canvas_detail::key(std::string(sv).c_str());
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_title(
    uint8_t tag, int64_t data) {
  auto sv = _culebra_str_view(tag, data);
  culebra::_canvas_detail::set_title(std::string(sv).c_str());
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_canvas_key_pop() {
  return _culebra_heap_str(culebra::_canvas_detail::key_pop());
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_canvas_char_pop() {
  return _culebra_heap_str(culebra::_canvas_detail::char_pop());
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_canvas_closing() {
  return culebra::_canvas_detail::closing();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_canvas_windowed() {
  return culebra::_canvas_detail::windowed();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_tone(
    int64_t start_freq, int64_t end_freq, int64_t attack, int64_t decay,
    int64_t sustain, int64_t release, int64_t vol, int64_t peak,
    int64_t channel, int64_t duty) {
  culebra::_canvas_detail::tone(start_freq, end_freq, attack, decay, sustain,
                                release, vol, peak, channel, duty);
}
// The MP3/Ogg sniff runs here, before the backend branch, mirroring interp —
// same ValueError, same site, on every backend (see stdlib_interp.h).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_music_play(
    uint8_t tag, int64_t data, int64_t looping, int64_t vol, double start,
    int64_t line, int64_t col) {
  auto sv = _culebra_str_view(tag, data);
  auto p = reinterpret_cast<const uint8_t*>(sv.data());
  const char* fmt = culebra::_canvas_detail::music_format(p, sv.size());
  if (fmt == nullptr)
    throw culebra::CulebraError(
        "ValueError", culebra::_canvas_detail::kMusicFormatError, line, col);
  culebra::_canvas_detail::music_play(p, static_cast<int64_t>(sv.size()), fmt,
                                      looping, vol, start);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_music_stop() {
  culebra::_canvas_detail::music_stop();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_music_pause() {
  culebra::_canvas_detail::music_pause();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_music_resume() {
  culebra::_canvas_detail::music_resume();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_music_volume(
    int64_t vol) {
  culebra::_canvas_detail::music_volume(vol);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_music_seek(
    double seconds) {
  culebra::_canvas_detail::music_seek(seconds);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_canvas_music_playing() {
  return culebra::_canvas_detail::music_playing();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_sound_load(
    uint8_t tag, int64_t data, int64_t line, int64_t col) {
  auto sv = _culebra_str_view(tag, data);
  auto p = reinterpret_cast<const uint8_t*>(sv.data());
  const char* fmt = culebra::_canvas_detail::sound_format(p, sv.size());
  if (fmt == nullptr)
    throw culebra::CulebraError(
        "ValueError", culebra::_canvas_detail::kSoundFormatError, line, col);
  int64_t id = culebra::_canvas_detail::sound_alloc_id();
  culebra::_canvas_detail::sound_load(id, p, static_cast<int64_t>(sv.size()),
                                      fmt);
  return id;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_sound_play(
    int64_t id, int64_t vol) {
  culebra::_canvas_detail::sound_play(id, vol);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_sound_stop(
    int64_t id) {
  culebra::_canvas_detail::sound_stop(id);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_canvas_sound_playing(
    int64_t id) {
  return culebra::_canvas_detail::sound_playing(id);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_canvas_sound_free(
    int64_t id) {
  culebra::_canvas_detail::sound_free(id);
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_width() {
  return culebra::_canvas_detail::width();
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_canvas_height() {
  return culebra::_canvas_detail::height();
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
  if (!culebra::os_strptime(s ? s : "", fmt ? fmt : "", &tm)) {
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

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_sys_getcwd(
    int64_t line, int64_t col) {
  std::error_code ec;
  auto p = std::filesystem::current_path(ec);
  if (ec) _fs_throw_io("Sys.getcwd", line, col, ec);
  return _culebra_heap_str(p.string());
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_sys_chdir(
    const char* path, int64_t line, int64_t col) {
  std::error_code ec;
  std::filesystem::current_path(std::filesystem::path(path ? path : ""), ec);
  if (ec) _fs_throw_io(
      std::format("Sys.chdir('{}')", path ? path : ""), line, col, ec);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_sys_set_env(
    const char* name, const char* value, int64_t line, int64_t col) {
  if (culebra::os_setenv(name ? name : "", value ? value : "", 1) != 0) {
    std::error_code ec(errno, std::generic_category());
    _fs_throw_io(std::format("Sys.set_env('{}')", name ? name : ""),
                 line, col, ec);
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_sys_argv() {
  auto& argv = culebra::sys_argv();
  auto* r = culebra_runtime_array_new();
  for (const auto& s : argv) {
    auto* str = _culebra_heap_str(s);
    culebra_runtime_array_push(r, TAG_STRING,
                               reinterpret_cast<int64_t>(str));
  }
  return r;
}

// --- JSON ---

// JSON logic lives in json.h (culebra::json), shared with the interp
// (stdlib_interp.h) so values, error texts, and positions stay
// byte-identical across backends. These policies adapt the neutral core
// to JitValues.

struct _JitJsonReader {
  using Value = JitValue;
  static culebra::json::Kind kind(const JitValue& v) {
    using culebra::json::Kind;
    switch (v.tag) {
      case TAG_NIL:    return Kind::Nil;
      case TAG_BOOL:   return Kind::Bool;
      case TAG_LONG:   return Kind::Long;
      case TAG_FLOAT:  return Kind::Float;
      case TAG_STRING: return Kind::String;
      // Array and Tuple share JitArray storage; Set renders in insertion
      // order — all three are JSON arrays.
      case TAG_ARRAY:
      case TAG_TUPLE:
      case TAG_SET:    return Kind::Seq;
      case TAG_OBJECT: return Kind::Object;
    }
    return Kind::Other;
  }
  static bool as_bool(const JitValue& v) { return v.data != 0; }
  static int64_t as_long(const JitValue& v) { return v.data; }
  static double as_double(const JitValue& v) {
    return _culebra_float_to_double(v.data);
  }
  static std::string_view as_string(const JitValue& v) {
    return reinterpret_cast<const char*>(v.data);
  }
  static size_t seq_size(const JitValue& v) {
    if (v.tag == TAG_SET) return reinterpret_cast<JitSet*>(v.data)->members.size();
    return reinterpret_cast<JitArray*>(v.data)->size;
  }
  static const JitValue& seq_at(const JitValue& v, size_t i) {
    if (v.tag == TAG_SET) return reinterpret_cast<JitSet*>(v.data)->members[i];
    return reinterpret_cast<JitArray*>(v.data)->items[i];
  }
  static bool object_has_non_string_keys(const JitValue& v) {
    auto* obj = reinterpret_cast<JitObject*>(v.data);
    return obj->non_string_props && !obj->non_string_props->empty();
  }
  static std::vector<std::pair<std::string_view, const JitValue*>>
  object_entries(const JitValue& v) {
    auto* obj = reinterpret_cast<JitObject*>(v.data);
    std::vector<std::pair<std::string_view, const JitValue*>> entries;
    if (!obj->shape) return entries;
    entries.reserve(obj->shape->names.size());
    for (size_t i = 0; i < obj->shape->names.size(); i++) {
      entries.emplace_back(obj->shape->names[i], &obj->slots[i].value);
    }
    return entries;
  }
  static std::string_view type_name(const JitValue& v) {
    return _culebra_tag_name(v.tag);
  }
};

// Each built heap object (JitArray, JitObject, allocated String) is +1
// owned by the caller, who hands ownership to the user binding.
struct _JitJsonBuilder {
  using Value = JitValue;
  using Object = JitObject*;
  using Array = JitArray*;
  static JitValue make_null() { return {TAG_NIL, 0}; }
  static JitValue boolean(bool b) { return {TAG_BOOL, b ? 1 : 0}; }
  static JitValue integer(int64_t n) { return {TAG_LONG, n}; }
  static JitValue real(double d) {
    return {TAG_FLOAT, _culebra_double_to_bits(d)};
  }
  static JitValue string(std::string&& s) {
    return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(s))};
  }
  static JitObject* object_new() { return culebra_runtime_object_new(); }
  static void object_set(JitObject*& o, const std::string& key, JitValue&& v) {
    culebra_runtime_object_set(o, key.c_str(), false, v.tag, v.data, 0, 0);
  }
  static JitValue object_done(JitObject*&& o) {
    return {TAG_OBJECT, reinterpret_cast<int64_t>(o)};
  }
  static JitArray* array_new() { return culebra_runtime_array_new(); }
  static void array_push(JitArray*& a, JitValue&& v) {
    culebra_runtime_array_push(a, v.tag, v.data);
  }
  static JitValue array_done(JitArray*&& a) {
    return {TAG_ARRAY, reinterpret_cast<int64_t>(a)};
  }
  // Raw +1 heap objects: release the partial container / finished value
  // when a parse throws mid-way. The container abandons funnel through the
  // single value releaser so there is exactly one bare RC site here (the
  // release is a no-op for non-heap tags).
  static void object_abandon(JitObject*& o) {
    JitValue v{TAG_OBJECT, reinterpret_cast<int64_t>(o)};
    abandon_value(v);
  }
  static void array_abandon(JitArray*& a) {
    JitValue v{TAG_ARRAY, reinterpret_cast<int64_t>(a)};
    abandon_value(v);
  }
  static void abandon_value(JitValue& v) {
    culebra_runtime_value_release(v.tag, v.data);
  }
};

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

// A `share` Object (name -> SharedBuffer) resolved for a child process, in
// two halves so each caller keeps its own cleanup discipline: collect the
// (name, buffer id) pairs while nothing has been allocated yet, then turn
// them into the CULEBRA_SHARE_* env entries and the fds the child inherits.
// Borrows the Object. Shared by the kwarg resolver and the positional
// adapters below — `share:` reached only the resolver before, so a call the
// compile-time keyword peephole did not handle (the VM's direct call, or
// `let f = Proc.run; f(cmd, share: ...)` on any backend) dropped it silently
// and the child could not attach the buffer.
inline std::string _ns_proc_share_ids(
    JitValue raw, std::string_view ctx,
    std::vector<std::pair<std::string, long>>& out) {
  if (raw.tag != TAG_OBJECT)
    return std::string(ctx) +
           ": share must be an Object of name -> SharedBuffer";
  auto* obj = reinterpret_cast<JitObject*>(raw.data);
  if (!obj->shape) return {};
  for (size_t k = 0; k < obj->shape->names.size(); k++) {
    const std::string& name = obj->shape->names[k];
    const JitValue& bv = obj->slots[k].value;
    size_t si = (bv.tag == TAG_OBJECT)
                    ? reinterpret_cast<JitObject*>(bv.data)
                          ->find_slot("__sharedbuffer_id__")
                    : static_cast<size_t>(-1);
    if (si == static_cast<size_t>(-1))
      return std::format("{}: share `{}` must be a SharedBuffer", ctx, name);
    out.emplace_back(
        name, reinterpret_cast<JitObject*>(bv.data)->slots[si].value.data);
  }
  return {};
}

inline void _ns_proc_share_apply(
    const std::vector<std::pair<std::string, long>>& ids,
    std::vector<std::pair<std::string, std::string>>& env_out,
    std::vector<int>& fds_out) {
  for (const auto& [name, id] : ids) {
    auto [fd, env_val] = culebra::prepare_share_buffer(id, name);
    env_out.emplace_back(culebra::share_env_key(name), std::move(env_val));
    fds_out.push_back(fd);
  }
}

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
      // build_merged threw mid-loop (both throw sites are in the splat
      // pass, which runs before any kwarg slot is moved into the map).
      // The map holds only splat-retained copies — release those, then
      // the caller-side +1s nobody consumed yet: every kwarg slot and
      // every splat operand. The JIT callers emit no slab cleanup of
      // their own (consume-at-store, callee-consumes), so this is
      // the sole releaser on the throw edge.
      release_merged();
      for (int64_t i = 0; i < n_kw; i++) {
        _culebra_value_release_impl(kw_vals[i].tag, kw_vals[i].data);
      }
      for (int64_t i = 0; i < n_splat; i++) {
        _culebra_value_release_impl(splat_objs[i].tag, splat_objs[i].data);
      }
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
    // Collect (name, buffer id) first so a validation throw can't leak `raw`.
    std::vector<std::pair<std::string, long>> entries;
    std::string err = _ns_proc_share_ids(raw, ctx_, entries);
    _culebra_value_release_impl(raw.tag, raw.data);
    if (!err.empty()) fail(err);
    _ns_proc_share_apply(entries, env_out, fds_out);
    return true;
  }

  // Throw if any kwargs went unread — i.e. unknown to this built-in.
  void validate_consumed() {
    if (merged_.empty()) return;
    fail(culebra::unknown_kwarg_message(culebra::canonical_unknown_kwarg(merged_)));
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
    return _culebra_heap_str(culebra::json::stringify<_JitJsonReader>(
        {tag, data}, static_cast<int>(indent), sort_keys != 0));
  }
  if (indent > 0) {
    throw culebra::CulebraError("TypeError",
        "JSON.stringify: lines=true is incompatible with indent>0");
  }
  return _culebra_heap_str(culebra::json::stringify_lines<_JitJsonReader>(
      {tag, data}, sort_keys != 0));
}

// Parse driver: the shared core builds JitValues directly through
// _JitJsonBuilder; the returned root is +1 owned by the caller, who
// hands ownership to the user binding.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_json_parse(
    const char* s, const char* number_mode, int8_t lines, int8_t jsonc) {
  if (!lines) {
    return culebra::json::parse<_JitJsonBuilder>(s, number_mode, jsonc != 0);
  }
  return culebra::json::parse_lines<_JitJsonBuilder>(s, number_mode,
                                                     jsonc != 0);
}

// Build the `{code, stdout, stderr, ok, signal, error}` result Object shared
// by Proc.run/all/race. Spawned => process result with `error` nil; a spawn
// failure => `ok:false` with the message in `error` (Proc.all's allSettled
// error representation).
inline JitObject* _culebra_proc_outcome_to_object(
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
inline JitValue _culebra_proc_run_impl(
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
inline JitValue _culebra_proc_all_impl(
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
inline JitValue _culebra_proc_race_impl(
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
    void (*fn)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*), size_t arity,
    const JitParamMeta* meta);

// `r.json()` — parse the response body as JSON. Method ABI: self arrives +1
// (release before returning), mirroring the proc/file handle methods.
CULEBRA_RT_INLINE void _jit_http_json(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                          JitValue*) {
  JitValue self{self_tag, self_data};
  auto* o = reinterpret_cast<JitObject*>(self.data);
  size_t i = o->find_slot("body");
  std::string body;
  if (i != static_cast<size_t>(-1)) {
    JitValue b = o->slots[i].value;
    if (b.tag == TAG_STRING || b.tag == TAG_STRINGVIEW) {
      body = _culebra_str_view(b.tag, b.data);
    }
  }
  JitValue r = culebra_runtime_json_parse(body.c_str(), "auto", 0, 0);
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = r; return; }
}

// Build the `{status, ok, body, headers}` response Object from an HttpResult,
// plus a `json()` method. `ok` is 2xx; a 4xx/5xx is a completed round-trip
// (ok:false). Transport failures never reach here — _culebra_http_run throws
// HttpError first.
inline JitObject* _culebra_http_result_to_object(
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

// As _http_run_into, but against a persistent Http.client (id). Mirrors interp
// http_run_client_into.
inline JitValue _http_run_client_into(int64_t id, culebra::http::HttpRequest& req,
                                      JitHttpInto& st, const char* ctx) {
  auto r = culebra::http::http_client_request(id, req);
  if (st.eptr) std::rethrow_exception(st.eptr);
  if (!r.ok) {
    throw culebra::CulebraError("HttpError", std::format("{}: {}", ctx, r.error),
                                0, 0);
  }
  return {TAG_OBJECT,
          reinterpret_cast<int64_t>(_culebra_http_result_to_object(r, 0, 0))};
}

// Parse the `files` slab value into req.multipart for a multipart/form-data
// upload. Mirrors interp http_setup_multipart: each Object entry is a part whose
// value is a String (text field), an Object ({content|path|stream, filename?,
// content_type?}), or an Array of those (repeated parts under one name). A
// path/stream part is streamed chunked so a big file or slow output never has to
// live in memory all at once.
inline void _http_setup_multipart(JitValue filesv,
                                  culebra::http::HttpRequest& req,
                                  JitHttpInto& st, const char* ctx) {
  if (filesv.tag != TAG_OBJECT) {
    throw culebra::CulebraError("TypeError",
        std::format("{}: files must be an Object", ctx), 0, 0);
  }
  auto str_at = [&](JitObject* o, const char* key, bool& present) -> std::string {
    size_t i = o->find_slot(key);
    present = i != static_cast<size_t>(-1);
    if (!present) return std::string();
    JitValue v = o->slots[i].value;
    if (v.tag != TAG_STRING && v.tag != TAG_STRINGVIEW) {
      throw culebra::CulebraError("TypeError",
          std::format("{}: files {} must be a String", ctx, key), 0, 0);
    }
    return std::string(_culebra_str_view(v.tag, v.data));
  };
  auto add_part = [&](const std::string& name, JitValue v) {
    culebra::http::MultipartPart part;
    part.name = name;
    if (v.tag == TAG_STRING || v.tag == TAG_STRINGVIEW) {  // bare String → field
      part.content = std::string(_culebra_str_view(v.tag, v.data));
      req.multipart.push_back(std::move(part));
      return;
    }
    if (v.tag != TAG_OBJECT) {
      throw culebra::CulebraError("TypeError",
          std::format("{}: files['{}'] must be a String, Object, or Array",
                      ctx, name), 0, 0);
    }
    auto* o = reinterpret_cast<JitObject*>(v.data);
    bool dummy;
    part.filename = str_at(o, "filename", dummy);
    part.content_type = str_at(o, "content_type", dummy);
    // Detect the body source by presence only, count first, then type-check the
    // selected field inline — so the message + check order match interp exactly
    // (str_at would relocate the check earlier and drop the per-part name).
    size_t content_slot = o->find_slot("content");
    size_t path_slot = o->find_slot("path");
    size_t stream_slot = o->find_slot("stream");
    bool has_content = content_slot != static_cast<size_t>(-1);
    bool has_path = path_slot != static_cast<size_t>(-1);
    bool has_stream = stream_slot != static_cast<size_t>(-1);
    if (has_content + has_path + has_stream != 1) {
      throw culebra::CulebraError("TypeError",
          std::format(
              "{}: files['{}'] needs exactly one of content, path, stream",
              ctx, name), 0, 0);
    }
    if (has_content) {
      JitValue cv = o->slots[content_slot].value;
      if (cv.tag != TAG_STRING && cv.tag != TAG_STRINGVIEW) {
        throw culebra::CulebraError("TypeError",
            std::format("{}: files['{}'].content must be a String", ctx, name),
            0, 0);
      }
      part.content = std::string(_culebra_str_view(cv.tag, cv.data));
    } else if (has_path) {
      JitValue pv = o->slots[path_slot].value;
      if (pv.tag != TAG_STRING && pv.tag != TAG_STRINGVIEW) {
        throw culebra::CulebraError("TypeError",
            std::format("{}: files['{}'].path must be a String", ctx, name), 0,
            0);
      }
      std::string path(_culebra_str_view(pv.tag, pv.data));
      part.source = culebra::http::make_file_source(path);
      if (!part.source) {
        throw culebra::CulebraError("IOError",
            std::format("{}: cannot open '{}' for reading", ctx, path), 0, 0);
      }
      if (part.filename.empty()) {
        part.filename = std::filesystem::path(path).filename().string();
      }
    } else {  // stream: producer Function
      JitValue sv = o->slots[stream_slot].value;
      if (sv.tag != TAG_FUNC) {
        throw culebra::CulebraError("TypeError",
            std::format("{}: files['{}'].stream must be a Function (producer)",
                        ctx, name), 0, 0);
      }
      auto* producer = reinterpret_cast<JitClosure*>(sv.data);
      part.source = [producer, &st, ctx](std::string& out) -> bool {
        if (st.eptr) return false;  // a previous part already failed.
        try {
          JitValue r = _culebra_invoke0(producer);
          if (r.tag == TAG_NIL) return false;  // end of stream.
          if (r.tag != TAG_STRING && r.tag != TAG_STRINGVIEW) {
            _culebra_value_release_impl(r.tag, r.data);
            throw culebra::CulebraError("TypeError",
                std::format(
                    "{}: file stream producer must return a String or nil", ctx),
                0, 0);
          }
          out = _culebra_str_view(r.tag, r.data);
          _culebra_value_release_impl(r.tag, r.data);
          return true;
        } catch (...) {
          st.eptr = std::current_exception();  // abort; rethrown after send.
          return false;
        }
      };
    }
    req.multipart.push_back(std::move(part));
  };
  auto* files = reinterpret_cast<JitObject*>(filesv.data);
  if (files->shape) {
    for (size_t k = 0; k < files->shape->names.size(); k++) {
      std::string name(files->shape->names[k]);
      JitValue v = files->slots[k].value;
      if (v.tag == TAG_ARRAY) {
        auto* arr = reinterpret_cast<JitArray*>(v.data);
        for (size_t j = 0; j < arr->size; j++) add_part(name, arr->items[j]);
      } else {
        add_part(name, v);
      }
    }
  }
}

// Configure the request body from the `body` slab value: TAG_STRING → whole;
// TAG_FUNC → producer (called per chunk, returns next chunk String or nil),
// streamed chunked. `json` → application/json; `form` → urlencoded; `files` →
// multipart/form-data. Else → TypeError. Mirrors interp http_setup_body.
inline void _http_setup_body(JitValue bodyv, JitValue jsonv, JitValue formv,
                             JitValue filesv, JitValue ct,
                             culebra::http::HttpRequest& req, JitHttpInto& st,
                             const char* ctx) {
  // At most one of body / json / form / files. `json` → application/json;
  // `form` → application/x-www-form-urlencoded; `files` → multipart/form-data.
  bool has_json = jsonv.tag != TAG_NIL;
  bool has_form = formv.tag != TAG_NIL;
  bool has_files = filesv.tag != TAG_NIL;
  bool has_body =
      bodyv.tag == TAG_FUNC ||
      ((bodyv.tag == TAG_STRING || bodyv.tag == TAG_STRINGVIEW) &&
       !std::string_view(_culebra_str_view(bodyv.tag, bodyv.data)).empty());
  if (has_json + has_form + has_files + has_body > 1) {
    throw culebra::CulebraError(
        "TypeError",
        std::format("{}: pass at most one of body, json, form, files", ctx), 0,
        0);
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
  if (has_files) {
    // Content-Type (multipart/form-data; boundary=...) is set by the http core.
    _http_setup_multipart(filesv, req, st, ctx);
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
// method-closure slots; each method reads `self` (the handle) via the JitFn
// `self` argument. Result is cached on first wait/poll (idempotent); `drop`
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
CULEBRA_RT_INLINE void _jit_handle_wait(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                            int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
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
  { *__ret = ret; return; }
}
CULEBRA_RT_INLINE void _jit_handle_poll(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                            int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
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
  { *__ret = ret; return; }
}
CULEBRA_RT_INLINE void _jit_handle_kill(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                            int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int sig = (n >= 1 && args[0].tag == TAG_LONG)
                ? static_cast<int>(args[0].data) : 15;
  if (!_jit_handle_done(h)) {
    culebra::proc::kill_pid(_jit_handle_long(h, "_pid"), sig);
  }
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_handle_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                            int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  auto* h = reinterpret_cast<JitObject*>(self.data);
  if (_jit_handle_done(h)) { *__ret = {TAG_NIL, 0}; return; }
  int out_fd = static_cast<int>(_jit_handle_long(h, "_out"));
  int err_fd = static_cast<int>(_jit_handle_long(h, "_err"));
  int64_t pid = _jit_handle_long(h, "_pid");
  culebra::proc::kill_pid(pid, SIGKILL);
  culebra::proc::wait_handle(pid, out_fd, err_fd);
  h->set_or_append("_done", JitValue{TAG_BOOL, 1}, true);
  { *__ret = {TAG_NIL, 0}; return; }
}

CULEBRA_RT_INLINE JitValue _culebra_proc_build_handle(int64_t pid, int out_fd,
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
      "ArityError", culebra::missing_required_arg_message(pname),
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

CULEBRA_RT_INLINE void _jit_file_read(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                          int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int64_t id = _jit_handle_long(h, "_id");
  // `n` is untyped with default nil (nil → rest of file), so a non-Long
  // is the interp's to_long() body error, not a param error.
  const bool has_n = _jit_file_arg_present(n, args, 0) &&
                     args[0].tag != TAG_NIL;
  if (has_n && args[0].tag != TAG_LONG)
    _jit_file_body_type_error(self, "Long", args[0].tag);
  std::string out = has_n ? culebra::_file_read_n(id, args[0].data,
                                                  _jit_call_site_line,
                                                  _jit_call_site_col)
                          : culebra::_file_read_all(id, _jit_call_site_line,
                                                    _jit_call_site_col);
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(out))}; return; }
}
CULEBRA_RT_INLINE void _jit_file_write(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                           int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int64_t id = _jit_handle_long(h, "_id");
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "data");
  // A `String` param rejects a StringView slice on every path (wrap.h's
  // jit_check_args predicate; interp type_matches) — same here.
  if (args[0].tag != TAG_STRING)
    _jit_file_param_type_error(self, "data", "String", 0);
  auto sv = _culebra_str_view(args[0].tag, args[0].data);
  culebra::_file_write(id, sv, _jit_call_site_line, _jit_call_site_col);
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_file_flush(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                           int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  culebra::_file_flush(_jit_handle_long(reinterpret_cast<JitObject*>(self.data),
                                        "_id"),
                       _jit_call_site_line, _jit_call_site_col);
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_file_seek(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                          int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int64_t id = _jit_handle_long(h, "_id");
  if (!_jit_file_arg_present(n, args, 0))
    _jit_file_missing_arg(self, "offset");
  if (args[0].tag != TAG_LONG)
    _jit_file_param_type_error(self, "offset", "Long", 0);
  int64_t off = args[0].data;
  // `whence` is untyped with default "set" — a non-String is the interp's
  // to_string() body error.
  std::string_view whence = "set";
  if (_jit_file_arg_present(n, args, 1)) {
    if (args[1].tag != TAG_STRING && args[1].tag != TAG_STRINGVIEW)
      _jit_file_body_type_error(self, "String", args[1].tag);
    whence = _culebra_str_view(args[1].tag, args[1].data);
  }
  culebra::_file_seek(id, off, whence, _jit_call_site_line,
                      _jit_call_site_col);
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_file_tell(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                          int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  int64_t pos = culebra::_file_tell(
      _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id"),
      _jit_call_site_line, _jit_call_site_col);
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_LONG, pos}; return; }
}
CULEBRA_RT_INLINE void _jit_file_close(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                           int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  culebra::_file_close(_jit_handle_long(reinterpret_cast<JitObject*>(self.data),
                                        "_id"));
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_file_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                          int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // drop runs from the destructor's drop protocol — must NOT release self.
  culebra::_file_close(_jit_handle_long(reinterpret_cast<JitObject*>(self.data),
                                        "_id"));
  { *__ret = {TAG_NIL, 0}; return; }
}

// lines()/chunks() iterator FastFns. captures: [handle_cell, id_cell,
// (n_cell,) line_cell, col_cell]. handle_cell keeps the File handle alive so
// an anonymous `File.open(p).lines()` isn't GC'd before the loop runs; the
// position is the one captured when the iterator was built, which is where the
// interp's closure reports a step's error too — not the loop that pulls it.
inline void _file_lines_fast_fn(JitClosure* cls, JitValue, bool* done,
                                int8_t* out_tag, int64_t* out_data) {
  int64_t id = cls->captures[1]->value.data;
  std::string out;
  if (!culebra::_file_getline(id, out, cls->captures[2]->value.data,
                              cls->captures[3]->value.data)) {
    *done = true;
    return;
  }
  *done = false;
  *out_tag = TAG_STRING;
  *out_data = reinterpret_cast<int64_t>(_culebra_heap_str(out));
}
inline void _file_chunks_fast_fn(JitClosure* cls, JitValue, bool* done,
                                 int8_t* out_tag, int64_t* out_data) {
  int64_t id = cls->captures[1]->value.data;
  int64_t n = cls->captures[2]->value.data;
  std::string chunk = culebra::_file_read_n(id, n, cls->captures[3]->value.data,
                                            cls->captures[4]->value.data);
  if (chunk.empty()) { *done = true; return; }
  *done = false;
  *out_tag = TAG_STRING;
  *out_data = reinterpret_cast<int64_t>(_culebra_heap_str(chunk));
}

// dispose() for a lines()/chunks() iterator: closes the handle by the id
// held in a capture cell. `self` here is the iterator wrapper (no `_id`
// property), so a handle-method thunk reading `self._id` silently no-ops —
// the interp twin captures the id in its dispose closure the same way.
inline void _file_iter_dispose_fn(JitValue* __ret, JitClosure* cls,
                                  int8_t self_tag, int64_t self_data,
                                  int64_t, JitValue*) {
  // Native iterator method: self arrives +1-owned (callee-consumes).
  JitOwnedVal self_guard(JitValue{self_tag, self_data});
  culebra::_file_close(cls->captures[0]->value.data);
  *__ret = {TAG_NIL, 0};
}

// Build an iterator over a File handle, adding a `dispose` method that
// closes the handle so a broken for-in still releases the fd.
inline JitObject* _file_iter_build(JitValue self, bool chunks, int64_t n) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int64_t id = _jit_handle_long(h, "_id");
  culebra_runtime_value_retain(self.tag, self.data);  // iter keeps handle alive
  auto* handle_cell = culebra_runtime_cell_new(self.tag, self.data);
  auto* id_cell = culebra_runtime_cell_new(TAG_LONG, id);
  // This call's site, not the pulling loop's: each step reports here.
  auto* line_cell = culebra_runtime_cell_new(TAG_LONG, _jit_call_site_line);
  auto* col_cell = culebra_runtime_cell_new(TAG_LONG, _jit_call_site_col);
  JitObject* it;
  if (chunks) {
    auto* n_cell = culebra_runtime_cell_new(TAG_LONG, n);
    it = _iter_wrap_fast<&_file_chunks_fast_fn>(
        {handle_cell, id_cell, n_cell, line_cell, col_cell});
  } else {
    it = _iter_wrap_fast<&_file_lines_fast_fn>(
        {handle_cell, id_cell, line_cell, col_cell});
  }
  _jit_register_native_fn(
      reinterpret_cast<const void*>(&_file_iter_dispose_fn));
  auto* dispose_cls = culebra_runtime_closure_new(
      reinterpret_cast<void*>(&_file_iter_dispose_fn), 1, 0);
  culebra_runtime_cell_retain(id_cell);  // wrap_fast's closures keep it alive
  dispose_cls->captures[0] = id_cell;
  it->set_or_append("dispose",
      JitValue{TAG_FUNC, reinterpret_cast<int64_t>(dispose_cls)}, false);
  return it;
}
CULEBRA_RT_INLINE void _jit_file_lines(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                           int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // dispose's close needs the handle; transfer self's +1 into the iterator
  // build (it retains again), then the iterator owns the keep-alive ref.
  auto* it = _file_iter_build(self, /*chunks=*/false, 0);
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
}
CULEBRA_RT_INLINE void _jit_file_chunks(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                            int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "n");
  if (args[0].tag != TAG_LONG)
    _jit_file_param_type_error(self, "n", "Long", 0);
  auto* it = _file_iter_build(self, /*chunks=*/true, args[0].data);
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
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

// --- FS.watch handle: iterable (the events) + close/drop. Mirrors interp
// make_watch_handle; both pull through the same fswatch core. --------------

// for-in pull: block for the next change. A close (or a stale id) ends the
// iteration; an interrupt propagates out as the cooperative Interrupted.
inline void _watch_events_fast_fn(JitClosure* cls, JitValue, bool* done,
                                  int8_t* out_tag, int64_t* out_data) {
  int64_t id = cls->captures[0]->value.data;
  culebra::fswatch::FsEvent ev;
  if (culebra::fswatch::fs_watch_next(id, ev) !=
      culebra::fswatch::Next::Event) {
    *done = true;
    return;
  }
  auto* o = culebra_runtime_object_new();
  o->set_or_append("path",
                   JitValue{TAG_STRING,
                            reinterpret_cast<int64_t>(_culebra_heap_str(ev.path))},
                   false);
  o->set_or_append(
      "kind",
      JitValue{TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(
                               culebra::fswatch::kind_name(ev.kind)))},
      false);
  *done = false;
  *out_tag = TAG_OBJECT;
  *out_data = reinterpret_cast<int64_t>(o);
}

CULEBRA_RT_INLINE void _jit_watch_iter(JitValue* __ret, JitClosure*, int8_t self_tag,
                                           int64_t self_data, int64_t, JitValue*) {
  // self arrives +1-owned (callee-consumes); the guard releases it on every
  // path, including the throw out of a failed iterator build.
  JitOwnedVal self(JitValue{self_tag, self_data});
  int64_t id =
      _jit_handle_long(reinterpret_cast<JitObject*>(self.borrow().data), "_id");
  auto* id_cell = culebra_runtime_cell_new(TAG_LONG, id);
  // No dispose: the handle owns the close, so `break` leaves a named handle
  // usable and an anonymous one is stopped by its own drop (interp twin).
  auto* it = _iter_wrap_fast<&_watch_events_fast_fn>({id_cell});
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
}
CULEBRA_RT_INLINE void _jit_watch_close(JitValue* __ret, JitClosure*, int8_t self_tag,
                                            int64_t self_data, int64_t, JitValue*) {
  JitOwnedVal self(JitValue{self_tag, self_data});
  culebra::fswatch::fs_watch_close(
      _jit_handle_long(reinterpret_cast<JitObject*>(self.borrow().data), "_id"));
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_watch_drop(JitValue* __ret, JitClosure*, int8_t self_tag,
                                           int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // drop runs from the destructor's drop protocol — must NOT release self.
  culebra::fswatch::fs_watch_close(
      _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id"));
  { *__ret = {TAG_NIL, 0}; return; }
}

CULEBRA_RT_INLINE JitValue _culebra_watch_build_handle(int64_t id) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_id", JitValue{TAG_LONG, id}, false);
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  _jit_handle_bind_method(h, "iter", _jit_watch_iter, 0);
  _jit_handle_bind_method(h, "close", _jit_watch_close, 0);
  _jit_handle_bind_method(h, "drop", _jit_watch_drop, 0);
  _jit_owned_bind_drop(h);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// --- IO.stdin() handle: a read-only reader over standard input, sharing the
// File reader shape (.lines() / .read(n=nil)). No fd/close — stdin isn't
// seekable or closeable, so it carries no resource and needs no drop. ------

// Per-step fast-fn for the .lines() iterator: yield each line (newline
// stripped) until EOF.
inline void _stdin_lines_fast_fn(JitClosure*, JitValue, bool* done,
                                 int8_t* out_tag, int64_t* out_data) {
  std::string out;
  if (!culebra::read_stdin_line_interruptible(out)) { *done = true; return; }
  *done = false;
  *out_tag = TAG_STRING;
  *out_data = reinterpret_cast<int64_t>(_culebra_heap_str(out));
}
CULEBRA_RT_INLINE void _jit_stdin_lines(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                            JitValue*) {
  JitValue self{self_tag, self_data};
  // The iterator reads the thread-local stdin buffer, not the handle, so no
  // keep-alive capture is needed — just consume the method ABI's +1 on self.
  auto* it = _iter_wrap_fast<&_stdin_lines_fast_fn>({});
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
}
// read(n=nil): nil → rest of stdin, Long → up to n bytes. A non-Long n is the
// interp's to_long() body error (mirrors _jit_file_read).
CULEBRA_RT_INLINE void _jit_stdin_read(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                           int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  const bool has_n = _jit_file_arg_present(n, args, 0) &&
                     args[0].tag != TAG_NIL;
  if (has_n && args[0].tag != TAG_LONG)
    _jit_file_body_type_error(self, "Long", args[0].tag);
  std::string out = has_n ? culebra::read_stdin_n_interruptible(
                                static_cast<size_t>(args[0].data))
                          : culebra::read_stdin_all_interruptible();
  culebra_runtime_value_release(self.tag, self.data);  // method ABI: self is +1
  { *__ret = {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(out))}; return; }
}
CULEBRA_RT_INLINE JitValue _culebra_stdin_build_handle() {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  static const JitParamMeta* read_meta = _jit_make_handle_meta({"n"}, {true});
  _jit_handle_bind_method(h, "read", _jit_stdin_read, 0, read_meta);
  _jit_handle_bind_method(h, "lines", _jit_stdin_lines, 0);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

#if defined(CULEBRA_SQLITE_ENABLED)
// ===========================================================================
// SQLite — JIT/AOT mirror of the interp Database/Statement handles (see
// stdlib_interp.h). The value-neutral cursor core (sqlite.h) is shared; only
// the value marshalling differs (JitValue/JitObject/JitArray here, Value/
// ObjectValue there). Kept byte-for-byte symmetric with the interp; the debug
// drift check (_check_canon_sigs_once) catches a forgotten JIT registration.
// ===========================================================================

[[noreturn]] CULEBRA_RT_INLINE void _jit_sqlite_throw(const std::string& msg,
                                                      int64_t line, int64_t col) {
  throw culebra::CulebraError("SQLiteError", std::format("SQLite: {}", msg),
                              line, col);
}

// culebra JitValue -> neutral BindVal (mirrors interp _sqlite_to_bind).
inline culebra::sqlite::BindVal _jit_sqlite_to_bind(JitValue v, int64_t line,
                                                    int64_t col) {
  using CT = culebra::sqlite::ColType;
  culebra::sqlite::BindVal b;
  switch (v.tag) {
    case TAG_NIL:
      b.type = CT::Null;
      break;
    case TAG_BOOL:
      b.type = CT::Integer;
      b.i = v.data ? 1 : 0;
      break;
    case TAG_LONG:
      b.type = CT::Integer;
      b.i = v.data;
      break;
    case TAG_FLOAT:
      b.type = CT::Float;
      b.d = _culebra_float_to_double(v.data);
      break;
    case TAG_STRING:
    case TAG_STRINGVIEW:
      b.type = CT::Text;
      b.text = _culebra_str_view(v.tag, v.data);
      break;
    default:
      throw culebra::CulebraError(
          "TypeError",
          std::format("SQLite: cannot bind a {} value", _culebra_tag_name(v.tag)),
          line, col);
  }
  return b;
}

// neutral Cell -> JitValue (runtime column-type mapping; mirrors interp).
inline JitValue _jit_sqlite_cell_to_value(const culebra::sqlite::Cell& c) {
  using CT = culebra::sqlite::ColType;
  switch (c.type) {
    case CT::Integer:
      return {TAG_LONG, c.i};
    case CT::Float:
      return culebra::jit_float(c.d);
    case CT::Text:
    case CT::Blob:
      return {TAG_STRING,
              reinterpret_cast<int64_t>(_culebra_heap_str(std::string(c.text)))};
    case CT::Null:
    default:
      return {TAG_NIL, 0};
  }
}

// Bind `params`: Array -> positional, Object -> named, nil -> none.
inline void _jit_sqlite_bind_params(int64_t stmt_id, JitValue params,
                                    int64_t line, int64_t col) {
  std::string err;
  if (params.tag == TAG_NIL) return;
  if (params.tag == TAG_ARRAY) {
    auto* arr = reinterpret_cast<JitArray*>(params.data);
    for (size_t i = 0; i < arr->size; i++) {
      auto b = _jit_sqlite_to_bind(arr->items[i], line, col);
      if (!culebra::sqlite::bind(stmt_id, static_cast<int>(i) + 1, b, &err))
        _jit_sqlite_throw(err, line, col);
    }
    return;
  }
  if (params.tag == TAG_OBJECT) {
    auto* obj = reinterpret_cast<JitObject*>(params.data);
    // A non-String key lives in the sidecar, not shape->names — reject it to
    // mirror the interp (which iterates every key and throws on a non-String).
    if (obj->non_string_props && !obj->non_string_props->empty()) {
      throw culebra::CulebraError(
          "TypeError", "SQLite: named parameter keys must be Strings", line, col);
    }
    if (obj->shape) {
      for (size_t k = 0; k < obj->shape->names.size(); k++) {
        const std::string& name = obj->shape->names[k];
        int pi = culebra::sqlite::bind_index(stmt_id, ":" + name);
        if (pi == 0) pi = culebra::sqlite::bind_index(stmt_id, "@" + name);
        if (pi == 0) pi = culebra::sqlite::bind_index(stmt_id, "$" + name);
        if (pi == 0)
          _jit_sqlite_throw(std::format("no such named parameter ':{}'", name),
                            line, col);
        auto b = _jit_sqlite_to_bind(obj->slots[k].value, line, col);
        if (!culebra::sqlite::bind(stmt_id, pi, b, &err))
          _jit_sqlite_throw(err, line, col);
      }
    }
    return;
  }
  throw culebra::CulebraError(
      "TypeError",
      std::format("SQLite: params must be an Array or Object, got {}",
                  _culebra_tag_name(params.tag)),
      line, col);
}

// Drive a statement to completion, collecting rows as Objects keyed by column.
inline JitValue _jit_sqlite_collect_rows(int64_t stmt_id, int64_t line,
                                         int64_t col) {
  auto* arr = culebra_runtime_array_new();
  int ncol = culebra::sqlite::column_count(stmt_id);
  std::vector<std::string> names(ncol);
  for (int i = 0; i < ncol; i++)
    names[i] = culebra::sqlite::column_name(stmt_id, i);
  std::string err;
  for (;;) {
    int rc = culebra::sqlite::step(stmt_id, &err);
    if (rc < 0) _jit_sqlite_throw(err, line, col);
    if (rc == 0) break;
    auto* row = culebra_runtime_object_new();
    for (int i = 0; i < ncol; i++) {
      JitValue v = _jit_sqlite_cell_to_value(culebra::sqlite::column(stmt_id, i));
      culebra_runtime_object_set(row, names[i].c_str(), false, v.tag, v.data,
                                 line, col);
    }
    culebra_runtime_array_push(arr, TAG_OBJECT, reinterpret_cast<int64_t>(row));
  }
  return {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
}

inline void _jit_sqlite_drain(int64_t stmt_id, int64_t line, int64_t col) {
  std::string err;
  for (;;) {
    int rc = culebra::sqlite::step(stmt_id, &err);
    if (rc < 0) _jit_sqlite_throw(err, line, col);
    if (rc == 0) break;
  }
}

// execute(db, sql, params) -> rows affected (transient statement).
inline JitValue _jit_sqlite_execute(int64_t db_id, const std::string& sql,
                                    JitValue params, int64_t line, int64_t col) {
  std::string err;
  int64_t st = culebra::sqlite::prepare(db_id, sql, &err);
  if (st < 0) _jit_sqlite_throw(err, line, col);
  try {
    _jit_sqlite_bind_params(st, params, line, col);
    _jit_sqlite_drain(st, line, col);
  } catch (...) {
    culebra::sqlite::finalize(st);
    throw;
  }
  culebra::sqlite::finalize(st);
  return {TAG_LONG, culebra::sqlite::changes(db_id)};
}

// query(db, sql, params) -> [Object] (transient statement).
inline JitValue _jit_sqlite_query(int64_t db_id, const std::string& sql,
                                  JitValue params, int64_t line, int64_t col) {
  std::string err;
  int64_t st = culebra::sqlite::prepare(db_id, sql, &err);
  if (st < 0) _jit_sqlite_throw(err, line, col);
  JitValue rows;
  try {
    _jit_sqlite_bind_params(st, params, line, col);
    rows = _jit_sqlite_collect_rows(st, line, col);
  } catch (...) {
    culebra::sqlite::finalize(st);
    throw;
  }
  culebra::sqlite::finalize(st);
  return rows;
}

// --- Statement handle methods (self arrives +1; released before return, except
// drop which runs from the destructor protocol). ---------------------------

CULEBRA_RT_INLINE int64_t _jit_sqlite_stmt_id(JitValue self) {
  return _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id");
}
// The owning Database id via the __parent__ slot.
CULEBRA_RT_INLINE int64_t _jit_sqlite_stmt_db_id(JitValue self) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  size_t pi = h->find_slot("__parent__");
  if (pi == static_cast<size_t>(-1)) return -1;
  auto* parent = reinterpret_cast<JitObject*>(h->slots[pi].value.data);
  return _jit_handle_long(parent, "_id");
}

CULEBRA_RT_INLINE void _jit_sqlite_stmt_run(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  int64_t st = _jit_sqlite_stmt_id(self);
  int64_t db = _jit_sqlite_stmt_db_id(self);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};  // method ABI: self is +1
  JitValue params = _jit_file_arg_present(n, args, 0) ? args[0] : JitValue{TAG_NIL, 0};
  culebra::sqlite::reset(st);
  _jit_sqlite_bind_params(st, params, _jit_call_site_line, _jit_call_site_col);
  _jit_sqlite_drain(st, _jit_call_site_line, _jit_call_site_col);
  { *__ret = JitValue{TAG_LONG, culebra::sqlite::changes(db)}; return; }
}
CULEBRA_RT_INLINE void _jit_sqlite_stmt_query(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                  int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  int64_t st = _jit_sqlite_stmt_id(self);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};  // method ABI: self is +1
  JitValue params = _jit_file_arg_present(n, args, 0) ? args[0] : JitValue{TAG_NIL, 0};
  culebra::sqlite::reset(st);
  _jit_sqlite_bind_params(st, params, _jit_call_site_line, _jit_call_site_col);
  { *__ret = _jit_sqlite_collect_rows(st, _jit_call_site_line, _jit_call_site_col); return; }
}
CULEBRA_RT_INLINE void _jit_sqlite_stmt_finalize(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                     int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  culebra::sqlite::finalize(_jit_sqlite_stmt_id(self));
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_sqlite_stmt_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                 int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // drop runs from the destructor's drop protocol — must NOT release self.
  culebra::sqlite::finalize(_jit_sqlite_stmt_id(self));
  { *__ret = {TAG_NIL, 0}; return; }
}

CULEBRA_RT_INLINE JitValue _culebra_sqlite_build_stmt_handle(int64_t stmt_id,
                                                             JitValue db_handle) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_id", JitValue{TAG_LONG, stmt_id}, false);
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  // Keep the owning Database alive while this Statement exists (mirrors the
  // interp handle's __parent__). Retain: the slot now co-owns the db handle.
  culebra_runtime_value_retain(db_handle.tag, db_handle.data);
  h->set_or_append("__parent__", db_handle, false);
  static const JitParamMeta* params_meta =
      _jit_make_handle_meta({"params"}, {true});
  _jit_handle_bind_method(h, "run", _jit_sqlite_stmt_run, 0, params_meta);
  _jit_handle_bind_method(h, "query", _jit_sqlite_stmt_query, 0, params_meta);
  _jit_handle_bind_method(h, "finalize", _jit_sqlite_stmt_finalize, 0);
  _jit_handle_bind_method(h, "drop", _jit_sqlite_stmt_drop, 0);
  _jit_owned_bind_drop(h);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// --- Database handle methods ------------------------------------------------

CULEBRA_RT_INLINE int64_t _jit_sqlite_db_id(JitValue self) {
  return _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id");
}

// Reads the required `sql` String arg (arity-checks + type-checks, releasing
// self on error like the File thunks), returning it by value.
CULEBRA_RT_INLINE std::string _jit_sqlite_take_sql(JitValue self, int64_t n,
                                                   JitValue* args) {
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "sql");
  if (args[0].tag != TAG_STRING)
    _jit_file_param_type_error(self, "sql", "String", 0);
  return std::string(_culebra_str_view(args[0].tag, args[0].data));
}

CULEBRA_RT_INLINE void _jit_sqlite_db_execute(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                  int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  int64_t db = _jit_sqlite_db_id(self);
  std::string sql = _jit_sqlite_take_sql(self, n, args);  // releases self on error
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  JitValue params = _jit_file_arg_present(n, args, 1) ? args[1] : JitValue{TAG_NIL, 0};
  { *__ret = _jit_sqlite_execute(db, sql, params, _jit_call_site_line,
                             _jit_call_site_col); return; }
}
CULEBRA_RT_INLINE void _jit_sqlite_db_query(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  int64_t db = _jit_sqlite_db_id(self);
  std::string sql = _jit_sqlite_take_sql(self, n, args);  // releases self on error
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  JitValue params = _jit_file_arg_present(n, args, 1) ? args[1] : JitValue{TAG_NIL, 0};
  { *__ret = _jit_sqlite_query(db, sql, params, _jit_call_site_line,
                           _jit_call_site_col); return; }
}
CULEBRA_RT_INLINE void _jit_sqlite_db_prepare(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                  int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  int64_t db = _jit_sqlite_db_id(self);
  std::string sql = _jit_sqlite_take_sql(self, n, args);  // releases self on error
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  std::string err;
  int64_t st = culebra::sqlite::prepare(db, sql, &err);
  if (st < 0) _jit_sqlite_throw(err, _jit_call_site_line, _jit_call_site_col);
  // build_stmt_handle retains self into the Statement's __parent__; the guard
  // still releases this method's +1.
  { *__ret = _culebra_sqlite_build_stmt_handle(st, self); return; }
}
CULEBRA_RT_INLINE void _jit_sqlite_db_transaction(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                      int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  int64_t db = _jit_sqlite_db_id(self);
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "fn");
  if (args[0].tag != TAG_FUNC)
    _jit_file_param_type_error(self, "fn", "Function", 0);
  auto* fn = reinterpret_cast<JitClosure*>(args[0].data);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  _jit_sqlite_execute(db, "BEGIN", JitValue{TAG_NIL, 0}, _jit_call_site_line,
                      _jit_call_site_col);
  try {
    // Owned across the COMMIT, which throws on a busy db or a deferred
    // constraint — the rollback arm below rethrows and would drop the +1.
    JitOwnedVal result(_culebra_invoke0(fn));
    _jit_sqlite_execute(db, "COMMIT", JitValue{TAG_NIL, 0}, _jit_call_site_line,
                        _jit_call_site_col);
    { *__ret = result.consume(); return; }
  } catch (...) {
    _jit_sqlite_execute(db, "ROLLBACK", JitValue{TAG_NIL, 0},
                        _jit_call_site_line, _jit_call_site_col);
    throw;
  }
}
CULEBRA_RT_INLINE void _jit_sqlite_db_close(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  culebra::sqlite::close_db(_jit_sqlite_db_id(self));
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_sqlite_db_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                               int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // drop runs from the destructor's drop protocol — must NOT release self.
  culebra::sqlite::close_db(_jit_sqlite_db_id(self));
  { *__ret = {TAG_NIL, 0}; return; }
}

CULEBRA_RT_INLINE JitValue _culebra_sqlite_build_db_handle(int64_t db_id) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_id", JitValue{TAG_LONG, db_id}, false);
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  static const JitParamMeta* exec_meta =
      _jit_make_handle_meta({"sql", "params"}, {false, true});
  static const JitParamMeta* sql_meta =
      _jit_make_handle_meta({"sql"}, {false});
  static const JitParamMeta* fn_meta = _jit_make_handle_meta({"fn"}, {false});
  _jit_handle_bind_method(h, "execute", _jit_sqlite_db_execute, 1, exec_meta);
  _jit_handle_bind_method(h, "query", _jit_sqlite_db_query, 1, exec_meta);
  _jit_handle_bind_method(h, "prepare", _jit_sqlite_db_prepare, 1, sql_meta);
  _jit_handle_bind_method(h, "transaction", _jit_sqlite_db_transaction, 1,
                          fn_meta);
  _jit_handle_bind_method(h, "close", _jit_sqlite_db_close, 0);
  _jit_handle_bind_method(h, "drop", _jit_sqlite_db_drop, 0);
  _jit_owned_bind_drop(h);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}
#endif  // CULEBRA_SQLITE_ENABLED

// ===========================================================================
// Net — JIT/AOT mirror of the interp Socket/Listener/UdpSocket handles (see
// stdlib_interp.h). The value-neutral socket core (net.h) is shared, framing
// and all: only the value marshalling differs (JitValue/JitObject here,
// Value/ObjectValue there). The debug drift check (_check_canon_sigs_once)
// catches a forgotten JIT registration.
// ===========================================================================

[[noreturn]] CULEBRA_RT_INLINE void _jit_net_throw(const char* ctx,
                                                   const std::string& msg,
                                                   int64_t line, int64_t col) {
  throw culebra::CulebraError("NetError", std::format("{}: {}", ctx, msg), line,
                              col);
}

// Mirrors the interp `_net_check`: Eof is the caller's business, only
// Timeout/Error become NetError (with the core's shared wording).
CULEBRA_RT_INLINE void _jit_net_check(culebra::net::IoStatus st, const char* ctx,
                                      const std::string& err, int64_t line,
                                      int64_t col) {
  if (st == culebra::net::IoStatus::Timeout ||
      st == culebra::net::IoStatus::Error) {
    _jit_net_throw(ctx, culebra::net::status_message(st, err), line, col);
  }
}

CULEBRA_RT_INLINE int64_t _jit_net_id(JitValue self) {
  return _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id");
}

CULEBRA_RT_INLINE JitValue _jit_net_str(const std::string& s) {
  return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(s))};
}

CULEBRA_RT_INLINE JitValue _jit_net_addr_object(const std::string& host,
                                                int port) {
  auto* o = culebra_runtime_object_new();
  o->set_or_append("host", _jit_net_str(host), false);
  o->set_or_append("port", JitValue{TAG_LONG, port}, false);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(o)};
}

// --- Shared handle methods (all three shapes) -------------------------------

CULEBRA_RT_INLINE void _jit_net_set_timeout(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "ms");
  if (args[0].tag != TAG_LONG)
    _jit_file_param_type_error(self, "ms", "Long", 0);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  std::string err;
  if (!culebra::net::set_timeout(_jit_net_id(self), args[0].data, &err))
    _jit_net_throw("Net.set_timeout", err, _jit_call_site_line,
                   _jit_call_site_col);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_net_is_open(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                            int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  bool open = culebra::net::is_open(_jit_net_id(self));
  { *__ret = {TAG_BOOL, open ? 1 : 0}; return; }
}
CULEBRA_RT_INLINE void _jit_net_close(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                          int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  culebra::net::close_handle(_jit_net_id(self));
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_net_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                         int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // drop runs from the destructor's drop protocol — must NOT release self.
  culebra::net::close_handle(_jit_net_id(self));
  { *__ret = {TAG_NIL, 0}; return; }
}

// Bind the `_id` + set_timeout/is_open/close/drop set every Net handle shares.
CULEBRA_RT_INLINE void _jit_net_bind_common(JitObject* h, int64_t id) {
  h->set_or_append("_id", JitValue{TAG_LONG, id}, false);
  // A native handle is not Sendable — reject it at the serialize boundary
  // (jit_serialize checks __nonsendable__), mirroring the interp handle.
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  static const JitParamMeta* ms_meta = _jit_make_handle_meta({"ms"}, {false});
  _jit_handle_bind_method(h, "set_timeout", _jit_net_set_timeout, 1, ms_meta);
  _jit_handle_bind_method(h, "is_open", _jit_net_is_open, 0);
  _jit_handle_bind_method(h, "close", _jit_net_close, 0);
  _jit_handle_bind_method(h, "drop", _jit_net_drop, 0);
  // Through the bind chokepoint, not a bare `has_drop`: the handle must also
  // register on the owned stack or a cycle-held socket would miss its
  // deterministic scope-exit drop.
  _jit_owned_bind_drop(h);
}

// --- Socket (connected TCP stream) ------------------------------------------

CULEBRA_RT_INLINE JitValue _culebra_net_build_socket_handle(int64_t id);

CULEBRA_RT_INLINE void _jit_net_sock_read(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                              int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  const bool has_n = _jit_file_arg_present(n, args, 0) && args[0].tag != TAG_NIL;
  if (has_n && args[0].tag != TAG_LONG)
    _jit_file_body_type_error(self, "Long", args[0].tag);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  std::string out, err;
  culebra::net::IoStatus st;
  if (has_n) {
    int64_t want = args[0].data;
    st = culebra::net::read(_jit_net_id(self),
                            static_cast<size_t>(want < 0 ? 0 : want), out, &err);
  } else {
    st = culebra::net::read_all(_jit_net_id(self), out, &err);
  }
  _jit_net_check(st, "Net.read", err, _jit_call_site_line, _jit_call_site_col);
  { *__ret = _jit_net_str(out); return; }  // Eof -> ""
}
CULEBRA_RT_INLINE void _jit_net_sock_read_line(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                   int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  std::string out, err;
  auto st = culebra::net::read_line(_jit_net_id(self), out, &err);
  if (st == culebra::net::IoStatus::Eof) { *__ret = {TAG_NIL, 0}; return; }
  _jit_net_check(st, "Net.read_line", err, _jit_call_site_line,
                 _jit_call_site_col);
  { *__ret = _jit_net_str(out); return; }
}
CULEBRA_RT_INLINE void _jit_net_sock_read_exact(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                    int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "n");
  if (args[0].tag != TAG_LONG) _jit_file_param_type_error(self, "n", "Long", 0);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  int64_t want = args[0].data;
  if (want < 0) want = 0;
  std::string out, err;
  auto st = culebra::net::read_exact(_jit_net_id(self),
                                     static_cast<size_t>(want), out, &err);
  if (st == culebra::net::IoStatus::Eof) {
    _jit_net_throw("Net.read_exact",
                   std::format("unexpected EOF ({} of {} bytes)", out.size(),
                               want),
                   _jit_call_site_line, _jit_call_site_col);
  }
  _jit_net_check(st, "Net.read_exact", err, _jit_call_site_line,
                 _jit_call_site_col);
  { *__ret = _jit_net_str(out); return; }
}

// lines() iterator FastFn. captures: [handle_cell, id_cell, line_cell,
// col_cell] — the handle keeps an anonymous `Net.connect(...).lines()` alive,
// and the captured call site keeps a mid-iteration NetError's position
// identical to the interp's (which captures it the same way).
inline void _net_lines_fast_fn(JitClosure* cls, JitValue, bool* done,
                               int8_t* out_tag, int64_t* out_data) {
  int64_t id = cls->captures[1]->value.data;
  std::string out, err;
  auto st = culebra::net::read_line(id, out, &err);
  if (st == culebra::net::IoStatus::Eof) { *done = true; return; }
  _jit_net_check(st, "Net.lines", err, cls->captures[2]->value.data,
                 cls->captures[3]->value.data);
  *done = false;
  *out_tag = TAG_STRING;
  *out_data = reinterpret_cast<int64_t>(_culebra_heap_str(out));
}
CULEBRA_RT_INLINE void _jit_net_sock_lines(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                               int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  int64_t id = _jit_net_id(self);
  // The method ABI hands self at +1; that reference moves straight into the
  // iterator's keep-alive cell (no retain/release pair needed, and no dispose
  // to close it — unlike File.lines a socket is usually written to after the
  // loop, mirroring the interp handle).
  auto* handle_cell = culebra_runtime_cell_new(self.tag, self.data);
  auto* id_cell = culebra_runtime_cell_new(TAG_LONG, id);
  auto* line_cell = culebra_runtime_cell_new(TAG_LONG, _jit_call_site_line);
  auto* col_cell = culebra_runtime_cell_new(TAG_LONG, _jit_call_site_col);
  auto* it = _iter_wrap_fast<&_net_lines_fast_fn>(
      {handle_cell, id_cell, line_cell, col_cell});
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
}

CULEBRA_RT_INLINE void _jit_net_sock_write(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                               int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "data");
  if (args[0].tag != TAG_STRING && args[0].tag != TAG_STRINGVIEW)
    _jit_file_param_type_error(self, "data", "String", 0);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  std::string_view data = _culebra_str_view(args[0].tag, args[0].data);
  std::string err;
  auto st = culebra::net::write_all(_jit_net_id(self), data.data(), data.size(),
                                    &err);
  _jit_net_check(st, "Net.write", err, _jit_call_site_line, _jit_call_site_col);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_net_sock_shutdown_write(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                        int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  std::string err;
  if (!culebra::net::shutdown_write(_jit_net_id(self), &err))
    _jit_net_throw("Net.shutdown_write", err, _jit_call_site_line,
                   _jit_call_site_col);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_net_sock_set_nodelay(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                     int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  const bool has_on = _jit_file_arg_present(n, args, 0);
  if (has_on && args[0].tag != TAG_BOOL)
    _jit_file_param_type_error(self, "on", "Bool", 0);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  std::string err;
  if (!culebra::net::set_nodelay(_jit_net_id(self),
                                 has_on ? args[0].data != 0 : true, &err))
    _jit_net_throw("Net.set_nodelay", err, _jit_call_site_line,
                   _jit_call_site_col);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_net_sock_local_addr(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                    int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  std::string host, err;
  int port = 0;
  if (!culebra::net::local_addr(_jit_net_id(self), host, port, &err))
    _jit_net_throw("Net.local_addr", err, _jit_call_site_line,
                   _jit_call_site_col);
  { *__ret = _jit_net_addr_object(host, port); return; }
}
CULEBRA_RT_INLINE void _jit_net_sock_peer_addr(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                   int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  std::string host, err;
  int port = 0;
  if (!culebra::net::peer_addr(_jit_net_id(self), host, port, &err))
    _jit_net_throw("Net.peer_addr", err, _jit_call_site_line,
                   _jit_call_site_col);
  { *__ret = _jit_net_addr_object(host, port); return; }
}

CULEBRA_RT_INLINE JitValue _culebra_net_build_socket_handle(int64_t id) {
  auto* h = culebra_runtime_object_new();
  _jit_net_bind_common(h, id);
  static const JitParamMeta* read_meta = _jit_make_handle_meta({"n"}, {true});
  static const JitParamMeta* exact_meta = _jit_make_handle_meta({"n"}, {false});
  static const JitParamMeta* write_meta =
      _jit_make_handle_meta({"data"}, {false});
  static const JitParamMeta* on_meta = _jit_make_handle_meta({"on"}, {true});
  _jit_handle_bind_method(h, "read", _jit_net_sock_read, 0, read_meta);
  _jit_handle_bind_method(h, "read_line", _jit_net_sock_read_line, 0);
  _jit_handle_bind_method(h, "read_exact", _jit_net_sock_read_exact, 1,
                          exact_meta);
  _jit_handle_bind_method(h, "lines", _jit_net_sock_lines, 0);
  _jit_handle_bind_method(h, "write", _jit_net_sock_write, 1, write_meta);
  _jit_handle_bind_method(h, "shutdown_write", _jit_net_sock_shutdown_write, 0);
  _jit_handle_bind_method(h, "set_nodelay", _jit_net_sock_set_nodelay, 0,
                          on_meta);
  _jit_handle_bind_method(h, "local_addr", _jit_net_sock_local_addr, 0);
  _jit_handle_bind_method(h, "peer_addr", _jit_net_sock_peer_addr, 0);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// --- Listener ---------------------------------------------------------------

CULEBRA_RT_INLINE void _jit_net_listener_accept(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                    int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  int64_t cid = -1;
  std::string err;
  auto st = culebra::net::accept(_jit_net_id(self), &cid, &err);
  _jit_net_check(st, "Net.accept", err, _jit_call_site_line,
                 _jit_call_site_col);
  { *__ret = _culebra_net_build_socket_handle(cid); return; }
}

// iter() FastFn — an endless accept loop; same captures as lines().
inline void _net_accept_fast_fn(JitClosure* cls, JitValue, bool* done,
                                int8_t* out_tag, int64_t* out_data) {
  int64_t id = cls->captures[1]->value.data;
  int64_t cid = -1;
  std::string err;
  auto st = culebra::net::accept(id, &cid, &err);
  _jit_net_check(st, "Net.accept", err, cls->captures[2]->value.data,
                 cls->captures[3]->value.data);
  JitValue conn = _culebra_net_build_socket_handle(cid);
  *done = false;
  *out_tag = static_cast<int8_t>(conn.tag);
  *out_data = conn.data;
}
CULEBRA_RT_INLINE void _jit_net_listener_iter(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                  int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  int64_t id = _jit_net_id(self);
  // self's +1 moves into the keep-alive cell, as in lines() above.
  auto* handle_cell = culebra_runtime_cell_new(self.tag, self.data);
  auto* id_cell = culebra_runtime_cell_new(TAG_LONG, id);
  auto* line_cell = culebra_runtime_cell_new(TAG_LONG, _jit_call_site_line);
  auto* col_cell = culebra_runtime_cell_new(TAG_LONG, _jit_call_site_col);
  auto* it = _iter_wrap_fast<&_net_accept_fast_fn>(
      {handle_cell, id_cell, line_cell, col_cell});
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
}

// Per-worker handler for `listener.serve`: each worker thread deserializes the
// Sendable handler onto its own heap here, and the on_conn trampoline calls it.
// thread_local — one per worker thread. JitOwnedVal so teardown's reset()
// releases it (no hand-placed RC op), mirroring the interp's g_net_w_handler.
inline thread_local std::optional<JitOwnedVal> g_jit_net_w_handler;

CULEBRA_RT_INLINE void _jit_net_listener_serve(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                   int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "handler");
  if (args[0].tag != TAG_FUNC)
    _jit_file_param_type_error(self, "handler", "Function", 0);
  const bool has_workers = _jit_file_arg_present(n, args, 1);
  if (has_workers && args[1].tag != TAG_LONG)
    _jit_file_param_type_error(self, "workers", "Long", 1);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  int64_t id = _jit_net_id(self);
  int64_t workers = has_workers ? args[1].data : 0;

  // Serialize once here, so a non-Sendable handler is an error at serve() —
  // not a surprise on the first connection (mirrors the interp).
  auto snode = std::make_shared<culebra::sendable::SendNode>();
  try {
    culebra::JitSerCtx sc;
    *snode = culebra::jit_serialize(args[0], sc);
  } catch (culebra::CulebraError& e) {
    throw culebra::CulebraError(
        "SendError",
        std::format("Net.serve: handler is not Sendable: {}", e.what()),
        _jit_call_site_line, _jit_call_site_col);
  }
  culebra::net::ServeHooks hooks;
  hooks.setup = [snode]() {
    culebra::JitDeCtx dc;
    JitValue h = culebra::jit_deserialize(*snode, dc);  // +1
    // Held only in this C++ slot → pin so the GC backstop won't sweep it.
    _gc_heap().pin(reinterpret_cast<void*>(h.data));
    g_jit_net_w_handler.emplace(h);
  };
  hooks.teardown = []() {
    if (g_jit_net_w_handler) {
      _gc_heap().unpin(
          reinterpret_cast<void*>(g_jit_net_w_handler->borrow().data));
    }
    g_jit_net_w_handler.reset();  // RAII release of the deserialized handler
  };
  hooks.on_conn = [](int64_t sid) {
    auto* cb =
        reinterpret_cast<JitClosure*>(g_jit_net_w_handler->borrow().data);
    JitOwnedVal ret{_culebra_invoke1(  // invoke consumes the socket handle's +1
        cb, _culebra_net_build_socket_handle(sid))};
  };
  std::string err;
  if (!culebra::net::serve(id, static_cast<int>(workers), hooks, &err))
    _jit_net_throw("Net.serve", err, _jit_call_site_line, _jit_call_site_col);
  { *__ret = {TAG_NIL, 0}; return; }
}

CULEBRA_RT_INLINE JitValue _culebra_net_build_listener_handle(
    int64_t id, const std::string& host, int port) {
  auto* h = culebra_runtime_object_new();
  _jit_net_bind_common(h, id);
  h->set_or_append("host", _jit_net_str(host), false);
  h->set_or_append("port", JitValue{TAG_LONG, port}, false);
  static const JitParamMeta* serve_meta =
      _jit_make_handle_meta({"handler", "workers"}, {false, true});
  _jit_handle_bind_method(h, "accept", _jit_net_listener_accept, 0);
  _jit_handle_bind_method(h, "serve", _jit_net_listener_serve, 1, serve_meta);
  _jit_handle_bind_method(h, "iter", _jit_net_listener_iter, 0);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// --- UdpSocket --------------------------------------------------------------

CULEBRA_RT_INLINE void _jit_net_udp_send_to(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "data");
  if (args[0].tag != TAG_STRING && args[0].tag != TAG_STRINGVIEW)
    _jit_file_param_type_error(self, "data", "String", 0);
  if (!_jit_file_arg_present(n, args, 1)) _jit_file_missing_arg(self, "host");
  if (args[1].tag != TAG_STRING && args[1].tag != TAG_STRINGVIEW)
    _jit_file_param_type_error(self, "host", "String", 1);
  if (!_jit_file_arg_present(n, args, 2)) _jit_file_missing_arg(self, "port");
  if (args[2].tag != TAG_LONG)
    _jit_file_param_type_error(self, "port", "Long", 2);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  std::string_view data = _culebra_str_view(args[0].tag, args[0].data);
  std::string err;
  if (!culebra::net::udp_send_to(
          _jit_net_id(self), data.data(), data.size(),
          std::string(_culebra_str_view(args[1].tag, args[1].data)),
          static_cast<int>(args[2].data), &err))
    _jit_net_throw("Net.send_to", err, _jit_call_site_line, _jit_call_site_col);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_net_udp_recv_from(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                  int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  const bool has_max = _jit_file_arg_present(n, args, 0);
  if (has_max && args[0].tag != TAG_LONG)
    _jit_file_param_type_error(self, "max", "Long", 0);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  int64_t max = has_max ? args[0].data : 65536;
  if (max < 0) max = 0;
  std::string data, host, err;
  int port = 0;
  auto st = culebra::net::udp_recv_from(_jit_net_id(self),
                                        static_cast<size_t>(max), data, host,
                                        port, &err);
  _jit_net_check(st, "Net.recv_from", err, _jit_call_site_line,
                 _jit_call_site_col);
  auto* o = culebra_runtime_object_new();
  o->set_or_append("data", _jit_net_str(data), false);
  o->set_or_append("host", _jit_net_str(host), false);
  o->set_or_append("port", JitValue{TAG_LONG, port}, false);
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(o)}; return; }
}
CULEBRA_RT_INLINE void _jit_net_udp_set_broadcast(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                      int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  const bool has_on = _jit_file_arg_present(n, args, 0);
  if (has_on && args[0].tag != TAG_BOOL)
    _jit_file_param_type_error(self, "on", "Bool", 0);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  std::string err;
  if (!culebra::net::set_broadcast(_jit_net_id(self),
                                   has_on ? args[0].data != 0 : true, &err))
    _jit_net_throw("Net.set_broadcast", err, _jit_call_site_line,
                   _jit_call_site_col);
  { *__ret = {TAG_NIL, 0}; return; }
}

CULEBRA_RT_INLINE JitValue _culebra_net_build_udp_handle(int64_t id,
                                                         const std::string& host,
                                                         int port) {
  auto* h = culebra_runtime_object_new();
  _jit_net_bind_common(h, id);
  h->set_or_append("host", _jit_net_str(host), false);
  h->set_or_append("port", JitValue{TAG_LONG, port}, false);
  static const JitParamMeta* send_meta =
      _jit_make_handle_meta({"data", "host", "port"}, {false, false, false});
  static const JitParamMeta* recv_meta = _jit_make_handle_meta({"max"}, {true});
  static const JitParamMeta* on_meta = _jit_make_handle_meta({"on"}, {true});
  _jit_handle_bind_method(h, "send_to", _jit_net_udp_send_to, 3, send_meta);
  _jit_handle_bind_method(h, "recv_from", _jit_net_udp_recv_from, 0, recv_meta);
  _jit_handle_bind_method(h, "set_broadcast", _jit_net_udp_set_broadcast, 0,
                          on_meta);
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

// --- Compile-side dispatch (extension hook implementation) ---

namespace culebra {

// JitExtension fills the ExtensionHooks for the standard library
// (Math/IO/Random/Sys + bare globals like inspect/to_long/type_of and the
// `Math.range(N).<HOF>(...)` fusion peephole). Declared as a friend of
// JIT in jit.h so declare_runtime can reach the JIT internals it needs
// (builder_/module_/valueType_/make_*/extract_*/...) without the JIT
// class having to expose them publicly.
struct NsMethod;  // defined below; referenced by compile_ns_method_kwargs
// The per-method spec is the canonical signature row (canon_sigs.h);
// referenced by compile_single_positional_kwargs.
using NsParamMeta = CanonSig;

struct JitExtension {
#ifdef CULEBRA_JIT_ENABLED
  // Defined at the end of this header — the one member that emits IR.
  static void declare_runtime(JIT& jit);
#endif
  static bool is_builtin_var(const std::string& name);

  static void install() {
    // Omitted with no JIT: there is no module to declare on, the executor
    // calls the same helpers directly, and an omitted designated initializer
    // is value-initialized — which is the nullptr the hook documents.
    install_extension({
#ifdef CULEBRA_JIT_ENABLED
        .declare_runtime = &declare_runtime,
#endif
        .is_builtin_var = &is_builtin_var,
    });
  }
};

// Convenience wrapper for embedders. Call once before running a program.
inline void install_jit_stdlib() { JitExtension::install(); }

// --- Stdlib namespace as first-class JIT object ---
//
// `let m = IO; m.inspect(x)` needs IO to be a value, not just a
// compile-time syntactic pattern. Each stdlib namespace gets a
// lazy JitObject whose method slots are closures that trampoline
// into a single dispatcher table. Fast path (`IO.inspect(x)` as
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
// PathLike coercion for a path-typed adapter argument: a String/StringView
// passes through, a `Path` object collapses to its inner string via __str__
// (culebra_runtime_value_to_display honors it). The JIT twin of the interp
// `_fspath`; byte-identical "expected String|Path" wording keeps FS.read(42)
// symmetric across backends. Returns a stable C string (heap-interned for the
// Path case).
inline const char* take_path(JitValue v) {
  if (v.tag == TAG_STRING) return reinterpret_cast<const char*>(v.data);
  if (v.tag == TAG_STRINGVIEW ||
      _culebra_value_matches_type(v.tag, v.data, "Path"))
    return culebra_runtime_value_to_display(v.tag, v.data);
  culebra::throw_runtime_error_at(
      "TypeError",
      culebra::type_mismatch_message("String|Path",
                                     culebra_runtime_type_of(v.tag)),
      0, 0);
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
inline JitValue _ns_io_inspect(JitValue* a, int64_t) {
  culebra_runtime_inspect(a[0].tag, a[0].data);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_io_print(JitValue* a, int64_t) {
  culebra_runtime_print(a[0].tag, a[0].data);
  return _ns_adapt::v_nil();
}
// `arg` defaults to "" (Python-style bare `println()` prints a blank line) —
// mirrors the interp's kw_default_empty_str() default on the same param.
inline JitValue _ns_io_println(JitValue* a, int64_t n) {
  JitValue arg = n > 0 ? a[0]
                       : _ns_adapt::v_string(_culebra_heap_str(std::string()));
  culebra_runtime_println(arg.tag, arg.data);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_io_input(JitValue*, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_input());
}
inline JitValue _ns_io_stdin(JitValue*, int64_t) {
  return _culebra_stdin_build_handle();
}
inline JitValue _ns_io_einspect(JitValue* a, int64_t) {
  culebra_runtime_einspect(a[0].tag, a[0].data);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_io_eprint(JitValue* a, int64_t) {
  culebra_runtime_eprint(a[0].tag, a[0].data);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_io_eprintln(JitValue* a, int64_t) {
  culebra_runtime_eprintln(a[0].tag, a[0].data);
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

// Bare global builtins (`inspect`/`print`/`println` reuse the IO adapters above)
// exposed as first-class values: `let f = type_of`, `[1,2,3].map(type_of)`.
// Each delegates to the very runtime helper the direct-call fast path
// (compile_global) emits, so the produced value is byte-identical. line/col
// are 0 — the value/HOF call site can't thread a position through the closure
// ABI, matching the ns-method adapters above.
inline JitValue _ns_global_type_of(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_type_of(a[0].tag));
}
inline JitValue _ns_global_to_long(JitValue* a, int64_t n) {
  // interp's builtin reads __LINE__/__COLUMN__ (its call-site channel) and
  // throws the bad-conversion error WITH that position — not positionless,
  // so the dispatch's boundary backfill must not claim it. Mirror the read.
  return culebra_runtime_to_long_any(a[0].tag, a[0].data,
                                     _jit_call_site_line, _jit_call_site_col,
                                     n > 1 ? a[1].data : 10);
}
inline JitValue _ns_global_to_float(JitValue* a, int64_t) {
  return culebra_runtime_to_float_any(a[0].tag, a[0].data,
                                      _jit_call_site_line,
                                      _jit_call_site_col);
}
inline JitValue _ns_global_to_string(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_value_to_display(a[0].tag, a[0].data));
}
inline JitValue _ns_global_hash(JitValue* a, int64_t) {
  return _ns_adapt::v_long(
      culebra_runtime_hash_any(a[0].tag, a[0].data, 0, 0));
}
inline JitValue _ns_global_eff_copy(JitValue* a, int64_t) {
  if (a[0].tag != TAG_OBJECT) return a[0];   // symmetric with interp's passthrough
  return _ns_adapt::v_object(
      culebra_runtime_eff_copy(reinterpret_cast<JitObject*>(a[0].data)));
}
inline JitValue _ns_global_eff_abort(JitValue* a, int64_t) {
  culebra_runtime_eff_abort(static_cast<int8_t>(a[0].tag), a[0].data);
  return {TAG_NIL, 0};  // unreachable: eff_abort always throws
}
inline JitValue _ns_global_eff_catch_abort(JitValue* a, int64_t) {
  auto* fn = reinterpret_cast<JitClosure*>(a[0].data);
  return {TAG_ARRAY,
          reinterpret_cast<int64_t>(culebra_runtime_eff_catch_abort(fn))};
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
// repeat(n, value): materialize a new Array of `n` copies of `value`. Fixed
// arity 2 (unlike iota's variadic collect), so a[0]/a[1] are the raw args —
// but still just BORROWED (the generic ns-call slab drops every arg slot
// after the call returns, see compile_ns_call's emit_value_slab_call), so
// every copy needs its own retain; array_push absorbs the +1 it's given.
inline JitValue _ns_global_repeat(JitValue* a, int64_t) {
  int64_t n = _ns_adapt::take_long(a[0]);
  if (n < 0) {
    throw culebra::CulebraError("ValueError",
        "repeat() n must not be negative", 0, 0);
  }
  JitValue value = a[1];
  auto* r = culebra_runtime_array_new_reserved(n);
  for (int64_t i = 0; i < n; i++) {
    culebra_runtime_value_retain(value.tag, value.data);
    culebra_runtime_array_push(r, value.tag, value.data);
  }
  return _ns_adapt::v_array(r);
}
// grid as a first-class value: the collected positional Array must hold
// exactly 2 (Range) args — culebra_runtime_grid_new does the Range/bounds
// validation, same as the direct-call fast path. Line/col fall back to
// 0:0 like range/iota (the NsMethod adapter ABI carries no call-site
// position either).
inline JitValue _ns_global_grid(JitValue* a, int64_t) {
  auto* arr = reinterpret_cast<JitArray*>(a[0].data);
  int64_t cnt = arr->size;
  if (cnt != 2) {
    throw_runtime_error_at("ArityError",
        builtin_arity_error_message("grid", 2, 2, cnt), 0, 0);
  }
  auto x = arr->items[0];
  auto y = arr->items[1];
  return _ns_adapt::v_object(
      culebra_runtime_grid_new(x.tag, x.data, y.tag, y.data, 0, 0));
}
// Whole-file read/write convenience on FS (open+read/write+close). Reuses
// the runtime file helpers; streaming lives on the File handle.
inline JitValue _ns_fs_read(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_read_file(_ns_adapt::take_path(a[0]), 0, 0));
}
inline JitValue _ns_fs_write(JitValue* a, int64_t) {
  culebra_runtime_write_file(_ns_adapt::take_path(a[0]), a[1].tag, a[1].data,
                             0, 0);
  return _ns_adapt::v_nil();
}

// File.open(path, mode="r") -> handle. File.with(path, mode, fn) -> block value.
inline JitValue _ns_file_open(JitValue* a, int64_t n) {
  std::string path = _ns_adapt::take_path(a[0]);
  std::string mode = (n >= 2 && (a[1].tag == TAG_STRING ||
                                 a[1].tag == TAG_STRINGVIEW))
      ? std::string(_culebra_str_view(a[1].tag, a[1].data)) : "r";
  int64_t id = culebra::_file_open(path, mode, 0, 0);
  return _culebra_file_build_handle(id);
}
inline JitValue _ns_file_with(JitValue* a, int64_t n) {
  std::string path = _ns_adapt::take_path(a[0]);
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

#if defined(CULEBRA_SQLITE_ENABLED)
// SQLite.open(path) -> Database handle. SQLite.version() -> library version.
inline JitValue _ns_sqlite_open(JitValue* a, int64_t) {
  std::string path = _ns_adapt::take_str(a[0]);
  std::string err;
  int64_t id = culebra::sqlite::open_db(path, &err);
  if (id < 0) _jit_sqlite_throw(err, 0, 0);
  return _culebra_sqlite_build_db_handle(id);
}
inline JitValue _ns_sqlite_version(JitValue*, int64_t) {
  return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(
                          std::string(culebra::sqlite::libversion())))};
}
#endif  // CULEBRA_SQLITE_ENABLED

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
  return _ns_adapt::v_long(culebra::math::sign(_ns_adapt::take_long(a[0])));
}
inline JitValue _ns_math_clamp(JitValue* a, int64_t) {
  return culebra_runtime_math_clamp(a[0].tag, a[0].data, a[1].tag, a[1].data,
                                    a[2].tag, a[2].data, 0, 0);
}
inline JitValue _ns_math_wrap(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_math_wrap(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_long(a[1]), 0, 0));
}

// _Time / _Term / _Canvas — the three ABI primitive namespaces the user-facing
// `Time` / `Term` / `Canvas` modules are written against. compile_ns_call
// peepholes each method into a direct runtime call, which serves the syntactic
// `_Canvas.rect(...)` form; these adapters serve every other way the method can
// be reached — read as a value, called from the bytecode VM, handed to a
// higher-order function — so the namespace object resolves like any other.
// The declared types come from the canonical interp params (NsParamMeta), so
// each adapter only unwraps what its runtime twin takes.

// A geometry argument as a pixel index: `Long|Float`, where a Float rounds
// through canvas.h's shared rule (the peephole's value_to_canvas_coord).
inline int64_t _ns_coord(JitValue v) {
  return v.tag == TAG_LONG
             ? v.data
             : culebra_runtime_canvas_coord(_ns_adapt::take_double(v));
}

inline JitValue _ns_time_now_nanos(JitValue*, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_time_now_nanos());
}
inline JitValue _ns_time_monotonic(JitValue*, int64_t) {
  return _ns_adapt::v_float(culebra_runtime_time_monotonic());
}
inline JitValue _ns_time_sleep(JitValue* a, int64_t) {
  culebra_runtime_time_sleep(_ns_adapt::take_double(a[0]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_time_from_iso_nanos(JitValue* a, int64_t) {
  return _ns_adapt::v_long(
      culebra_runtime_time_from_iso_nanos(_ns_adapt::take_str(a[0]), 0, 0));
}
inline JitValue _ns_time_parse_nanos(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_time_parse_nanos(
      _ns_adapt::take_str(a[0]), _ns_adapt::take_str(a[1]), 0, 0));
}
inline JitValue _ns_time_iso_nanos(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_time_iso_nanos(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_bool(a[1])));
}
inline JitValue _ns_time_format_nanos(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_time_format_nanos(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_str(a[1]),
      _ns_adapt::take_bool(a[2])));
}
inline JitValue _ns_time_parts_nanos(JitValue* a, int64_t) {
  return _ns_adapt::v_object(culebra_runtime_time_parts_nanos(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_bool(a[1])));
}
inline JitValue _ns_time_from_parts_nanos(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_time_from_parts_nanos(
      _ns_adapt::take_object(a[0]), _ns_adapt::take_bool(a[1])));
}
inline JitValue _ns_time_weekday_nanos(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_time_weekday_nanos(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_bool(a[1])));
}
inline JitValue _ns_time_add_nanos(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_time_add_nanos(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_long(a[1]),
      _ns_adapt::take_long(a[2]), _ns_adapt::take_long(a[3]),
      _ns_adapt::take_long(a[4]), _ns_adapt::take_long(a[5]),
      _ns_adapt::take_long(a[6]), _ns_adapt::take_bool(a[7])));
}
inline JitValue _ns_time_start_of_nanos(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_time_start_of_nanos(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_str(a[1]),
      _ns_adapt::take_bool(a[2]), 0, 0));
}

inline JitValue _ns_term_cols(JitValue*, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_term_cols());
}
inline JitValue _ns_term_rows(JitValue*, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_term_rows());
}
inline JitValue _ns_term_raw_on(JitValue*, int64_t) {
  culebra_runtime_term_raw_on();
  return _ns_adapt::v_nil();
}
inline JitValue _ns_term_raw_off(JitValue*, int64_t) {
  culebra_runtime_term_raw_off();
  return _ns_adapt::v_nil();
}
inline JitValue _ns_term_flush(JitValue*, int64_t) {
  culebra_runtime_term_flush();
  return _ns_adapt::v_nil();
}
inline JitValue _ns_term_color_level(JitValue*, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_term_color_level());
}
inline JitValue _ns_term_width(JitValue* a, int64_t) {
  // StringLike: a StringView reaches the C ABI through the shared view.
  return _ns_adapt::v_long(culebra_runtime_term_width(
      std::string(_culebra_str_view(a[0].tag, a[0].data)).c_str()));
}
inline JitValue _ns_term_resized(JitValue*, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_term_resized());
}
inline JitValue _ns_term_read_key(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_term_read_key(_ns_adapt::take_double(a[0])));
}
inline JitValue _ns_term_attach_tty(JitValue*, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_term_attach_tty());
}

inline JitValue _ns_canvas_init(JitValue* a, int64_t) {
  culebra_runtime_canvas_init(_ns_adapt::take_long(a[0]),
                              _ns_adapt::take_long(a[1]), 0, 0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_clear(JitValue* a, int64_t) {
  culebra_runtime_canvas_clear(_ns_adapt::take_long(a[0]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_set_pixel(JitValue* a, int64_t) {
  culebra_runtime_canvas_set_pixel(_ns_coord(a[0]), _ns_coord(a[1]),
                                   _ns_adapt::take_long(a[2]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_get_pixel(JitValue* a, int64_t) {
  return _ns_adapt::v_long(
      culebra_runtime_canvas_get_pixel(_ns_coord(a[0]), _ns_coord(a[1])));
}
inline JitValue _ns_canvas_rect(JitValue* a, int64_t) {
  culebra_runtime_canvas_rect(_ns_coord(a[0]), _ns_coord(a[1]),
                              _ns_coord(a[2]), _ns_coord(a[3]),
                              _ns_adapt::take_long(a[4]),
                              _ns_adapt::take_long(a[5]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_line(JitValue* a, int64_t) {
  culebra_runtime_canvas_line(_ns_coord(a[0]), _ns_coord(a[1]),
                              _ns_coord(a[2]), _ns_coord(a[3]),
                              _ns_adapt::take_long(a[4]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_ellipse(JitValue* a, int64_t) {
  culebra_runtime_canvas_ellipse(_ns_coord(a[0]), _ns_coord(a[1]),
                                 _ns_coord(a[2]), _ns_coord(a[3]),
                                 _ns_adapt::take_long(a[4]),
                                 _ns_adapt::take_long(a[5]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_triangle(JitValue* a, int64_t) {
  culebra_runtime_canvas_triangle(
      _ns_coord(a[0]), _ns_coord(a[1]), _ns_coord(a[2]), _ns_coord(a[3]),
      _ns_coord(a[4]), _ns_coord(a[5]), _ns_adapt::take_long(a[6]),
      _ns_adapt::take_long(a[7]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_polygon(JitValue* a, int64_t) {
  culebra_runtime_canvas_polygon(_ns_adapt::take_array(a[0]),
                                 _ns_adapt::take_long(a[1]),
                                 _ns_adapt::take_long(a[2]), 0, 0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_font_load(JitValue* a, int64_t) {
  return _ns_adapt::v_long(
      culebra_runtime_canvas_font_load(_ns_adapt::take_array(a[0])));
}
inline JitValue _ns_canvas_glyph(JitValue* a, int64_t) {
  culebra_runtime_canvas_glyph(_ns_adapt::take_long(a[0]),
                               _ns_adapt::take_long(a[1]), _ns_coord(a[2]),
                               _ns_coord(a[3]), _ns_adapt::take_long(a[4]),
                               _ns_adapt::take_long(a[5]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_ttf_load(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_ttf_load(
      static_cast<uint8_t>(a[0].tag), a[0].data, 0, 0));
}
inline JitValue _ns_canvas_ttf_free(JitValue* a, int64_t) {
  culebra_runtime_canvas_ttf_free(_ns_adapt::take_long(a[0]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_ttf_glyph(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_ttf_glyph(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_long(a[1]), _ns_coord(a[2]),
      _ns_coord(a[3]), _ns_adapt::take_long(a[4]),
      _ns_adapt::take_long(a[5])));
}
inline JitValue _ns_canvas_ttf_glyph_screen(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_ttf_glyph_screen(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_long(a[1]), _ns_coord(a[2]),
      _ns_coord(a[3]), _ns_adapt::take_long(a[4]),
      _ns_adapt::take_long(a[5])));
}
inline JitValue _ns_canvas_ttf_advance(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_ttf_advance(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_long(a[1]),
      _ns_adapt::take_long(a[2])));
}
inline JitValue _ns_canvas_ttf_ascent(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_ttf_ascent(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_long(a[1])));
}
inline JitValue _ns_canvas_screen_width(JitValue*, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_screen_width());
}
inline JitValue _ns_canvas_screen_height(JitValue*, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_screen_height());
}
inline JitValue _ns_canvas_get_screen_pixel(JitValue* a, int64_t) {
  return _ns_adapt::v_long(
      culebra_runtime_canvas_get_screen_pixel(_ns_coord(a[0]), _ns_coord(a[1])));
}
inline JitValue _ns_canvas_screen_scale(JitValue*, int64_t) {
  return _ns_adapt::v_float(culebra_runtime_canvas_screen_scale());
}
inline JitValue _ns_canvas_sprite_load(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_sprite_load(
      _ns_adapt::take_array(a[0]), _ns_adapt::take_long(a[1]),
      _ns_adapt::take_long(a[2])));
}
inline JitValue _ns_canvas_sprite_from_png(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_sprite_from_png(
      static_cast<uint8_t>(a[0].tag), a[0].data, 0, 0));
}
inline JitValue _ns_canvas_sprite_to_png(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_canvas_sprite_to_png(_ns_adapt::take_long(a[0]), 0, 0));
}
inline JitValue _ns_canvas_sprite_width(JitValue* a, int64_t) {
  return _ns_adapt::v_long(
      culebra_runtime_canvas_sprite_width(_ns_adapt::take_long(a[0])));
}
inline JitValue _ns_canvas_sprite_height(JitValue* a, int64_t) {
  return _ns_adapt::v_long(
      culebra_runtime_canvas_sprite_height(_ns_adapt::take_long(a[0])));
}
inline JitValue _ns_canvas_sprite_blank(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_sprite_blank(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_long(a[1]),
      _ns_adapt::take_long(a[2])));
}
inline JitValue _ns_canvas_sprite_free(JitValue* a, int64_t) {
  culebra_runtime_canvas_sprite_free(_ns_adapt::take_long(a[0]), 0, 0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_target(JitValue* a, int64_t) {
  return _ns_adapt::v_long(
      culebra_runtime_canvas_target(_ns_adapt::take_long(a[0]), 0, 0));
}
inline JitValue _ns_canvas_blit(JitValue* a, int64_t) {
  culebra_runtime_canvas_blit(_ns_adapt::take_long(a[0]), _ns_coord(a[1]),
                              _ns_coord(a[2]), _ns_coord(a[3]),
                              _ns_coord(a[4]), _ns_coord(a[5]),
                              _ns_coord(a[6]), _ns_adapt::take_long(a[7]), 0,
                              0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_blit_scaled(JitValue* a, int64_t) {
  culebra_runtime_canvas_blit_scaled(
      _ns_adapt::take_long(a[0]), _ns_coord(a[1]), _ns_coord(a[2]),
      _ns_coord(a[3]), _ns_coord(a[4]), _ns_coord(a[5]), _ns_coord(a[6]),
      _ns_coord(a[7]), _ns_coord(a[8]), _ns_adapt::take_long(a[9]),
      _ns_adapt::take_long(a[10]), 0, 0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_present(JitValue*, int64_t) {
  culebra_runtime_canvas_present(0, 0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_buttons(JitValue*, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_buttons());
}
inline JitValue _ns_canvas_mouse_x(JitValue*, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_mouse_x());
}
inline JitValue _ns_canvas_mouse_y(JitValue*, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_mouse_y());
}
inline JitValue _ns_canvas_mouse_buttons(JitValue*, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_mouse_buttons());
}
inline JitValue _ns_canvas_key(JitValue* a, int64_t) {
  return _ns_adapt::v_bool(
      culebra_runtime_canvas_key(static_cast<uint8_t>(a[0].tag), a[0].data));
}
inline JitValue _ns_canvas_key_pop(JitValue*, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_canvas_key_pop());
}
inline JitValue _ns_canvas_char_pop(JitValue*, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_canvas_char_pop());
}
inline JitValue _ns_canvas_closing(JitValue*, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_canvas_closing());
}
inline JitValue _ns_canvas_windowed(JitValue*, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_canvas_windowed());
}
inline JitValue _ns_canvas_title(JitValue* a, int64_t) {
  culebra_runtime_canvas_title(static_cast<uint8_t>(a[0].tag), a[0].data);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_tone(JitValue* a, int64_t) {
  culebra_runtime_canvas_tone(
      _ns_adapt::take_long(a[0]), _ns_adapt::take_long(a[1]),
      _ns_adapt::take_long(a[2]), _ns_adapt::take_long(a[3]),
      _ns_adapt::take_long(a[4]), _ns_adapt::take_long(a[5]),
      _ns_adapt::take_long(a[6]), _ns_adapt::take_long(a[7]),
      _ns_adapt::take_long(a[8]), _ns_adapt::take_long(a[9]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_music_play(JitValue* a, int64_t) {
  culebra_runtime_canvas_music_play(
      static_cast<uint8_t>(a[0].tag), a[0].data, _ns_adapt::take_long(a[1]),
      _ns_adapt::take_long(a[2]), _ns_adapt::take_double(a[3]), 0, 0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_music_stop(JitValue*, int64_t) {
  culebra_runtime_canvas_music_stop();
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_music_pause(JitValue*, int64_t) {
  culebra_runtime_canvas_music_pause();
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_music_resume(JitValue*, int64_t) {
  culebra_runtime_canvas_music_resume();
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_music_volume(JitValue* a, int64_t) {
  culebra_runtime_canvas_music_volume(_ns_adapt::take_long(a[0]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_music_seek(JitValue* a, int64_t) {
  culebra_runtime_canvas_music_seek(_ns_adapt::take_double(a[0]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_music_playing(JitValue*, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_canvas_music_playing());
}
inline JitValue _ns_canvas_sound_load(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_sound_load(
      static_cast<uint8_t>(a[0].tag), a[0].data, 0, 0));
}
inline JitValue _ns_canvas_sound_play(JitValue* a, int64_t) {
  culebra_runtime_canvas_sound_play(_ns_adapt::take_long(a[0]),
                                    _ns_adapt::take_long(a[1]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_sound_stop(JitValue* a, int64_t) {
  culebra_runtime_canvas_sound_stop(_ns_adapt::take_long(a[0]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_sound_playing(JitValue* a, int64_t) {
  return _ns_adapt::v_bool(
      culebra_runtime_canvas_sound_playing(_ns_adapt::take_long(a[0])));
}
inline JitValue _ns_canvas_sound_free(JitValue* a, int64_t) {
  culebra_runtime_canvas_sound_free(_ns_adapt::take_long(a[0]));
  return _ns_adapt::v_nil();
}
inline JitValue _ns_canvas_width(JitValue*, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_width());
}
inline JitValue _ns_canvas_height(JitValue*, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_canvas_height());
}

// FS
inline JitValue _ns_fs_join(JitValue* a, int64_t n) {
  return _ns_adapt::v_string(culebra_runtime_fs_join(a, n, 0, 0));
}
inline JitValue _ns_fs_basename(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_fs_basename(_ns_adapt::take_path(a[0])));
}
inline JitValue _ns_fs_dirname(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_fs_dirname(_ns_adapt::take_path(a[0])));
}
inline JitValue _ns_fs_extension(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_fs_extension(_ns_adapt::take_path(a[0])));
}
inline JitValue _ns_fs_stem(JitValue* a, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_fs_stem(_ns_adapt::take_path(a[0])));
}
inline JitValue _ns_fs_exists(JitValue* a, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_io_exists(_ns_adapt::take_path(a[0])));
}
inline JitValue _ns_fs_is_file(JitValue* a, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_fs_is_file(_ns_adapt::take_path(a[0])));
}
inline JitValue _ns_fs_is_dir(JitValue* a, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_fs_is_dir(_ns_adapt::take_path(a[0])));
}
inline JitValue _ns_fs_size(JitValue* a, int64_t) {
  return _ns_adapt::v_long(culebra_runtime_fs_size(_ns_adapt::take_path(a[0]), 0, 0));
}
inline JitValue _ns_fs_list_dir(JitValue* a, int64_t) {
  return _ns_adapt::v_array(
      culebra_runtime_fs_list_dir(_ns_adapt::take_path(a[0]), 0, 0));
}
inline JitValue _ns_fs_mkdir(JitValue* a, int64_t) {
  culebra_runtime_fs_mkdir(_ns_adapt::take_path(a[0]), 0, 0);
  return _ns_adapt::v_nil();
}
// remove(path, recursive=false): slab is full-arity via NsParamMeta.
inline JitValue _ns_fs_remove(JitValue* a, int64_t n) {
  JitValue rec = n > 1 ? a[1] : JitValue{TAG_BOOL, 0};
  if (rec.tag == TAG_BOOL && rec.data != 0) {
    culebra_runtime_fs_remove_all(_ns_adapt::take_path(a[0]), 0, 0);
  } else {
    culebra_runtime_fs_remove(_ns_adapt::take_path(a[0]), 0, 0);
  }
  return _ns_adapt::v_nil();
}
inline JitValue _ns_fs_stat(JitValue* a, int64_t) {
  return _ns_adapt::v_object(
      culebra_runtime_fs_stat(_ns_adapt::take_path(a[0]), 0, 0));
}
inline JitValue _ns_fs_rename(JitValue* a, int64_t) {
  culebra_runtime_fs_rename(_ns_adapt::take_path(a[0]),
                            _ns_adapt::take_path(a[1]), 0, 0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_fs_chmod(JitValue* a, int64_t) {
  culebra_runtime_fs_chmod(_ns_adapt::take_path(a[0]),
                           _ns_adapt::take_long(a[1]), 0, 0);
  return _ns_adapt::v_nil();
}
// FS.chown owner/group slot -> id (-1 = unchanged). nil/Long/String(name);
// any other type is a TypeError, an unknown name an IOError — wording matches
// the interp resolver. Positions are 0/0 (the trampoline backfills the call
// site).
inline int64_t _fs_chown_id(JitValue v, const char* param, bool is_user) {
  if (v.tag == TAG_NIL) return -1;
  if (v.tag == TAG_LONG) return v.data;
  if (v.tag == TAG_STRING || v.tag == TAG_STRINGVIEW) {
    std::string name(_culebra_str_view(v.tag, v.data));
    int64_t id = is_user ? culebra::_fs_uid_from_name(name)
                      : culebra::_fs_gid_from_name(name);
    if (id < 0) {
      culebra::throw_runtime_error_at(
          "IOError",
          std::format("FS.chown: unknown {} '{}'",
                      is_user ? "user" : "group", name),
          0, 0);
    }
    return id;
  }
  culebra::throw_runtime_error_at(
      "TypeError",
      std::format("type error: parameter '{}' expects String, Long, or Nil",
                  param),
      0, 0);
  return -1;  // unreachable
}
// chown(path, owner=nil, group=nil): slab full-arity via NsParamMeta.
inline JitValue _ns_fs_chown(JitValue* a, int64_t n) {
  int64_t uid = _fs_chown_id(n > 1 ? a[1] : JitValue{TAG_NIL, 0}, "owner", true);
  int64_t gid = _fs_chown_id(n > 2 ? a[2] : JitValue{TAG_NIL, 0}, "group", false);
  culebra::_fs_do_chown(_ns_adapt::take_path(a[0]), uid, gid, 0, 0);
  return _ns_adapt::v_nil();
}
// watch(path, recursive=true, match=nil): slab full-arity via NsParamMeta.
inline JitValue _ns_fs_watch(JitValue* a, int64_t n) {
  JitValue rec = n > 1 ? a[1] : JitValue{TAG_BOOL, 1};
  JitValue match = n > 2 ? a[2] : JitValue{TAG_NIL, 0};
  std::vector<std::string> exts;
  if (match.tag != TAG_NIL) {
    ::JitArray* arr = _ns_adapt::take_array(match);
    if (!arr) throw_type_error_at(0, 0);
    for (size_t i = 0; i < arr->size; i++) {
      if (arr->items[i].tag != ::TAG_STRING &&
          arr->items[i].tag != ::TAG_STRINGVIEW)
        throw_type_error_at(0, 0);
      exts.push_back(culebra::fswatch::normalize_ext(
          _culebra_str_view(arr->items[i].tag, arr->items[i].data)));
    }
  }
  std::string err;
  int64_t id = culebra::fswatch::fs_watch_open(
      _ns_adapt::take_path(a[0]),
      !(rec.tag == TAG_BOOL && rec.data == 0), exts, err);
  if (id < 0) culebra::_io_throw(err, 0, 0);
  return _culebra_watch_build_handle(id);
}

// copy(src, dst, recursive=false): slab full-arity via NsParamMeta.
inline JitValue _ns_fs_copy(JitValue* a, int64_t n) {
  JitValue rec = n > 2 ? a[2] : JitValue{TAG_BOOL, 0};
  culebra_runtime_fs_copy(_ns_adapt::take_path(a[0]), _ns_adapt::take_path(a[1]),
                          rec.tag == TAG_BOOL && rec.data != 0 ? 1 : 0, 0, 0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_fs_normpath(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_fs_normpath(_ns_adapt::take_path(a[0])));
}
inline JitValue _ns_fs_is_abs(JitValue* a, int64_t) {
  return _ns_adapt::v_bool(culebra_runtime_fs_is_abs(_ns_adapt::take_path(a[0])));
}
inline JitValue _ns_fs_abspath(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_fs_abspath(_ns_adapt::take_path(a[0]), 0, 0));
}
inline JitValue _ns_fs_realpath(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_fs_realpath(_ns_adapt::take_path(a[0]), 0, 0));
}
inline JitValue _ns_fs_is_symlink(JitValue* a, int64_t) {
  return _ns_adapt::v_bool(
      culebra_runtime_fs_is_symlink(_ns_adapt::take_path(a[0])));
}
inline JitValue _ns_fs_symlink(JitValue* a, int64_t) {
  culebra_runtime_fs_symlink(_ns_adapt::take_path(a[0]),
                             _ns_adapt::take_path(a[1]), 0, 0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_fs_readlink(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      culebra_runtime_fs_readlink(_ns_adapt::take_path(a[0]), 0, 0));
}
inline JitValue _ns_fs_walk(JitValue* a, int64_t) {
  return _ns_adapt::v_array(
      culebra_runtime_fs_walk(_ns_adapt::take_path(a[0]), 0, 0));
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
inline JitValue _ns_random_choice(JitValue* a, int64_t) {
  return culebra_runtime_random_choice(_ns_adapt::take_array(a[0]), 0, 0);
}

// Sys
// The doctest runner's exit guard: while set, Sys.exit throws a catchable
// ExitError (the interp runner's install_doctest_exit_guard wording) instead
// of terminating the process — a doc block calling Sys.exit must fail its
// block, not kill the whole run. Process-global like the runner itself.
inline bool& doctest_exit_guard() {
  static bool on = false;
  return on;
}
inline JitValue _ns_sys_exit(JitValue* a, int64_t) {
  int64_t code = _ns_adapt::take_long(a[0]);
  if (doctest_exit_guard()) {
    throw culebra::CulebraError("ExitError",
                                std::format("Sys.exit({}) called", code));
  }
  culebra_runtime_sys_exit(code);
  return _ns_adapt::v_nil();
}
// Kwarg slab: a[0]=name, a[1]=fallback ("" default). getenv is consulted
// directly rather than through culebra_runtime_sys_env because that one folds
// "unset" into "", which is exactly the distinction `fallback` restores.
inline JitValue _ns_sys_env(JitValue* a, int64_t n) {
  const char* v = std::getenv(_ns_adapt::take_str(a[0]));
  if (v) return _ns_adapt::v_string(_culebra_heap_str(std::string(v)));
  if (n > 1) return JitOwnedVal::from_borrowed(a[1]).consume();
  return _ns_adapt::v_string(_culebra_heap_str(std::string()));
}
inline JitValue _ns_sys_time(JitValue*, int64_t) {
  return _ns_adapt::v_float(culebra_runtime_sys_time());
}
inline JitValue _ns_sys_getcwd(JitValue*, int64_t) {
  return _ns_adapt::v_string(culebra_runtime_sys_getcwd(0, 0));
}
inline JitValue _ns_sys_chdir(JitValue* a, int64_t) {
  culebra_runtime_sys_chdir(_ns_adapt::take_str(a[0]), 0, 0);
  return _ns_adapt::v_nil();
}
inline JitValue _ns_sys_set_env(JitValue* a, int64_t) {
  culebra_runtime_sys_set_env(_ns_adapt::take_str(a[0]),
                              _ns_adapt::take_str(a[1]), 0, 0);
  return _ns_adapt::v_nil();
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
  int64_t rc_live = static_cast<int64_t>(gc.rc_live_count());
  int64_t bytes = static_cast<int64_t>(gc.live_bytes());
  auto* obj = culebra_runtime_object_new();
  culebra_runtime_object_set(obj, "live_objects", /*mut=*/false, TAG_LONG,
                             live, 0, 0);
  // Refcounted-only live count (Strings/StringViews excluded). The RC-leak
  // fuzzer measures this so benign traced-only churn under CULEBRA_GC_NEVER
  // (collector off) doesn't read as growth. Under normal GC it equals a
  // subset of live_objects; the interp exposes the same field (== its
  // live_objects, all heap objects are shared_ptr-managed) for symmetry.
  culebra_runtime_object_set(obj, "rc_objects", /*mut=*/false, TAG_LONG,
                             rc_live, 0, 0);
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

// The `share` slot of a positional slab: the last canonical parameter of
// every Proc entry point. Borrows the Object; the dispatch releases the
// slab afterwards. Returns whether anything was shared, so the caller knows
// to hand the impl its env overrides.
inline bool share_slot(JitValue sv, std::string_view ctx,
                       std::vector<std::pair<std::string, std::string>>& env,
                       std::vector<int>& fds) {
  if (sv.tag == TAG_NIL) return false;
  std::vector<std::pair<std::string, long>> ids;
  std::string err = _ns_proc_share_ids(sv, ctx, ids);
  if (!err.empty()) culebra::throw_runtime_error_at("TypeError", err, 0, 0);
  _ns_proc_share_apply(ids, env, fds);
  return true;
}

}  // namespace _proc_adapt

// --- Net ---------------------------------------------------------------------
// Net.connect / listen / udp build handles; Net.resolve returns [String]. The
// defaults restated here (timeout 0, host "0.0.0.0", backlog 0) match the
// canonical interp params — a nil slab slot means the caller omitted it.

inline JitValue _ns_net_connect(JitValue* a, int64_t n) {
  std::string host(_ns_adapt::require_sv(a[0], "host"));
  int64_t port = _ns_adapt::require_long(_proc_adapt::at(a, n, 1), "port");
  JitValue tv = _proc_adapt::at(a, n, 2);
  int64_t timeout =
      tv.tag == TAG_NIL ? 0 : _ns_adapt::require_long(tv, "timeout");
  std::string err;
  int64_t id =
      culebra::net::connect(host, static_cast<int>(port), timeout, &err);
  if (id < 0) _jit_net_throw("Net.connect", err, 0, 0);
  return _culebra_net_build_socket_handle(id);
}

// Read back the address a fresh listener / UDP socket actually bound, so the
// handle reports the ephemeral port a port-0 bind was given (mirrors the interp
// `_net_bound_addr`).
inline void _ns_net_bound_addr(int64_t id, const char* ctx, std::string& host,
                               int& port) {
  std::string err;
  if (!culebra::net::local_addr(id, host, port, &err)) {
    culebra::net::close_handle(id);
    _jit_net_throw(ctx, err, 0, 0);
  }
}

inline JitValue _ns_net_listen(JitValue* a, int64_t n) {
  int64_t port = _ns_adapt::require_long(a[0], "port");
  JitValue hv = _proc_adapt::at(a, n, 1);
  std::string host = hv.tag == TAG_NIL
                         ? std::string("0.0.0.0")
                         : std::string(_ns_adapt::require_sv(hv, "host"));
  JitValue bv = _proc_adapt::at(a, n, 2);
  int64_t backlog =
      bv.tag == TAG_NIL ? 0 : _ns_adapt::require_long(bv, "backlog");
  std::string err;
  int64_t id = culebra::net::listen(host, static_cast<int>(port),
                                    static_cast<int>(backlog), &err);
  if (id < 0) _jit_net_throw("Net.listen", err, 0, 0);
  std::string bound_host;
  int bound_port = 0;
  _ns_net_bound_addr(id, "Net.listen", bound_host, bound_port);
  return _culebra_net_build_listener_handle(id, bound_host, bound_port);
}

inline JitValue _ns_net_udp(JitValue* a, int64_t n) {
  JitValue pv = _proc_adapt::at(a, n, 0);
  int64_t port = pv.tag == TAG_NIL ? 0 : _ns_adapt::require_long(pv, "port");
  JitValue hv = _proc_adapt::at(a, n, 1);
  std::string host = hv.tag == TAG_NIL
                         ? std::string("0.0.0.0")
                         : std::string(_ns_adapt::require_sv(hv, "host"));
  std::string err;
  int64_t id = culebra::net::udp_open(host, static_cast<int>(port), &err);
  if (id < 0) _jit_net_throw("Net.udp", err, 0, 0);
  std::string bound_host;
  int bound_port = 0;
  _ns_net_bound_addr(id, "Net.udp", bound_host, bound_port);
  return _culebra_net_build_udp_handle(id, bound_host, bound_port);
}

inline JitValue _ns_net_resolve(JitValue* a, int64_t) {
  std::string host(_ns_adapt::require_sv(a[0], "host"));
  std::vector<std::string> addrs;
  std::string err;
  if (!culebra::net::resolve(host, addrs, &err))
    _jit_net_throw("Net.resolve", err, 0, 0);
  auto* arr = culebra_runtime_array_new();
  for (const auto& s : addrs) {
    culebra_runtime_array_push(
        arr, TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(s)));
  }
  return {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
}

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
  std::vector<int> fds;
  bool has_share =
      _proc_adapt::share_slot(_proc_adapt::at(a, n, 6), "Proc.run", over, fds);
  if (has_share) env = &over;
  return _culebra_proc_run_impl(a[0].tag, a[0].data, cwd, env, stdin_data,
                                check, timeout, 0, 0,
                                fds.empty() ? nullptr : &fds);
}
inline JitValue _ns_proc_all(JitValue* a, int64_t n) {
  // slab: commands, limit, timeout, fail_fast, retries
  JitValue lim = _proc_adapt::at(a, n, 1);
  JitValue to = _proc_adapt::at(a, n, 2);
  JitValue ff = _proc_adapt::at(a, n, 3);
  JitValue rt = _proc_adapt::at(a, n, 4);
  std::vector<std::pair<std::string, std::string>> over;
  std::vector<int> fds;
  bool has_share =
      _proc_adapt::share_slot(_proc_adapt::at(a, n, 5), "Proc.all", over, fds);
  return _culebra_proc_all_impl(
      a[0].tag, a[0].data, lim.tag == TAG_LONG ? lim.data : 0,
      to.tag == TAG_LONG ? to.data : 0, ff.tag == TAG_BOOL && ff.data != 0,
      rt.tag == TAG_LONG ? rt.data : 0, 0, 0, has_share ? &over : nullptr,
      fds.empty() ? nullptr : &fds);
}
inline JitValue _ns_proc_race(JitValue* a, int64_t n) {
  std::vector<std::pair<std::string, std::string>> over;
  std::vector<int> fds;
  bool has_share =
      _proc_adapt::share_slot(_proc_adapt::at(a, n, 1), "Proc.race", over, fds);
  return _culebra_proc_race_impl(a[0].tag, a[0].data, 0, 0,
                                 has_share ? &over : nullptr,
                                 fds.empty() ? nullptr : &fds);
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
                   _proc_adapt::at(a, n, 9), _proc_adapt::at(a, n, 10),
                   _proc_adapt::at(a, n, 2), req, st, ctx);
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
                   _proc_adapt::at(a, n, 10), _proc_adapt::at(a, n, 11),
                   _proc_adapt::at(a, n, 3), req, st, "Http.request");
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

// --- Http.client persistent-client handle (JIT) ---
// Mirrors interp make_http_client_handle: methods take a base-url-relative path
// (no per-request timeout/follow), reuse the client's connection, and layer its
// default headers under each request. The handle owns a +1 on `self`; the
// required-arg checks release it on error (file-thunk convention), then a
// _JitValueGuard releases it on every later exit (normal or throw).
CULEBRA_RT_INLINE int64_t _jit_http_client_id(JitValue self) {
  return _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id");
}

// Parse an Object-of-String slab value (headers/params) into `out`, matching
// interp http_parse_headers/params messages. No self release — the caller's
// _JitValueGuard covers the throw.
inline void _jit_http_client_strobj(JitValue v, culebra::http::HeaderList& out,
                                    const char* ctx, const char* container,
                                    const char* valueword) {
  if (v.tag == TAG_NIL) return;
  if (v.tag != TAG_OBJECT) {
    throw culebra::CulebraError("TypeError",
        std::format("{}: {} must be an Object of String", ctx, container), 0, 0);
  }
  if (!_ns_env_object_pairs(reinterpret_cast<JitObject*>(v.data), out)) {
    throw culebra::CulebraError("TypeError",
        std::format("{}: {} values must be String", ctx, valueword), 0, 0);
  }
}

inline JitValue _jit_http_client_bodyless(JitValue self, int64_t n,
                                          JitValue* args, const char* method,
                                          const char* ctx) {
  int64_t id = _jit_http_client_id(self);
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "path");
  // typed `String` param: reject StringView too (matches interp type_matches,
  // which only accepts "String" for a plain String annotation).
  if (args[0].tag != TAG_STRING)
    _jit_file_param_type_error(self, "path", "String", 0);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  auto at = [&](size_t i) {
    return _jit_file_arg_present(n, args, i) ? args[i] : JitValue{TAG_NIL, 0};
  };
  return _jit_at_call_site([&] {
    culebra::http::HttpRequest req;
    req.method = method;
    req.url = std::string(_culebra_str_view(args[0].tag, args[0].data));
    _jit_http_client_strobj(at(1), req.headers, ctx, "headers", "header");
    _jit_http_client_strobj(at(2), req.params, ctx, "params", "param");
    JitHttpInto st;
    _http_setup_into(at(3), req, st, ctx);
    return _http_run_client_into(id, req, st, ctx);
  });
}

inline JitValue _jit_http_client_withbody(JitValue self, int64_t n,
                                          JitValue* args, const char* method,
                                          const char* ctx) {
  int64_t id = _jit_http_client_id(self);
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "path");
  // typed `String` param: reject StringView too (matches interp type_matches,
  // which only accepts "String" for a plain String annotation).
  if (args[0].tag != TAG_STRING)
    _jit_file_param_type_error(self, "path", "String", 0);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  auto at = [&](size_t i) {
    return _jit_file_arg_present(n, args, i) ? args[i] : JitValue{TAG_NIL, 0};
  };
  return _jit_at_call_site([&] {
    culebra::http::HttpRequest req;
    req.method = method;
    req.url = std::string(_culebra_str_view(args[0].tag, args[0].data));
    _jit_http_client_strobj(at(3), req.headers, ctx, "headers", "header");
    _jit_http_client_strobj(at(4), req.params, ctx, "params", "param");
    JitHttpInto st;
    _http_setup_body(at(1), at(6), at(7), at(8), at(2), req, st, ctx);
    _http_setup_into(at(5), req, st, ctx);
    return _http_run_client_into(id, req, st, ctx);
  });
}

CULEBRA_RT_INLINE void _jit_http_client_get(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_client_bodyless(self, n, args, "GET", "client.get"); return; }
}
CULEBRA_RT_INLINE void _jit_http_client_delete(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                   int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_client_bodyless(self, n, args, "DELETE", "client.delete"); return; }
}
CULEBRA_RT_INLINE void _jit_http_client_head(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                 int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_client_bodyless(self, n, args, "HEAD", "client.head"); return; }
}
CULEBRA_RT_INLINE void _jit_http_client_post(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                 int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_client_withbody(self, n, args, "POST", "client.post"); return; }
}
CULEBRA_RT_INLINE void _jit_http_client_put(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_client_withbody(self, n, args, "PUT", "client.put"); return; }
}
CULEBRA_RT_INLINE void _jit_http_client_request(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                    int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  int64_t id = _jit_http_client_id(self);
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "method");
  // typed `String` params: reject StringView too (matches interp type_matches).
  if (args[0].tag != TAG_STRING)
    _jit_file_param_type_error(self, "method", "String", 0);
  if (!_jit_file_arg_present(n, args, 1)) _jit_file_missing_arg(self, "path");
  if (args[1].tag != TAG_STRING)
    _jit_file_param_type_error(self, "path", "String", 1);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  auto at = [&](size_t i) {
    return _jit_file_arg_present(n, args, i) ? args[i] : JitValue{TAG_NIL, 0};
  };
  *__ret = _jit_at_call_site([&] {
    culebra::http::HttpRequest req;
    req.method = std::string(_culebra_str_view(args[0].tag, args[0].data));
    req.url = std::string(_culebra_str_view(args[1].tag, args[1].data));
    _jit_http_client_strobj(at(4), req.headers, "client.request", "headers",
                            "header");
    _jit_http_client_strobj(at(5), req.params, "client.request", "params",
                            "param");
    JitHttpInto st;
    _http_setup_body(at(2), at(7), at(8), at(9), at(3), req, st,
                     "client.request");
    _http_setup_into(at(6), req, st, "client.request");
    return _http_run_client_into(id, req, st, "client.request");
  });
}
CULEBRA_RT_INLINE void _jit_http_client_close(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                  int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  culebra::http::http_client_close(_jit_http_client_id(self));
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_http_client_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                 int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // drop runs from the destructor's drop protocol — must NOT release self.
  culebra::http::http_client_close(_jit_http_client_id(self));
  { *__ret = {TAG_NIL, 0}; return; }
}

CULEBRA_RT_INLINE JitValue _culebra_http_build_client_handle(int64_t id) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_id", JitValue{TAG_LONG, id}, false);
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  static const JitParamMeta* bodyless_meta = _jit_make_handle_meta(
      {"path", "headers", "params", "into"}, {false, true, true, true});
  static const JitParamMeta* withbody_meta = _jit_make_handle_meta(
      {"path", "body", "content_type", "headers", "params", "into", "json",
       "form", "files"},
      {false, true, true, true, true, true, true, true, true});
  static const JitParamMeta* request_meta = _jit_make_handle_meta(
      {"method", "path", "body", "content_type", "headers", "params", "into",
       "json", "form", "files"},
      {false, false, true, true, true, true, true, true, true, true});
  _jit_handle_bind_method(h, "get", _jit_http_client_get, 1, bodyless_meta);
  _jit_handle_bind_method(h, "delete", _jit_http_client_delete, 1,
                          bodyless_meta);
  _jit_handle_bind_method(h, "head", _jit_http_client_head, 1, bodyless_meta);
  _jit_handle_bind_method(h, "post", _jit_http_client_post, 1, withbody_meta);
  _jit_handle_bind_method(h, "put", _jit_http_client_put, 1, withbody_meta);
  _jit_handle_bind_method(h, "request", _jit_http_client_request, 2,
                          request_meta);
  _jit_handle_bind_method(h, "close", _jit_http_client_close, 0);
  _jit_handle_bind_method(h, "drop", _jit_http_client_drop, 0);
  _jit_owned_bind_drop(h);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// Http.client(base_url, headers=nil, timeout=0, follow_redirects=true) → handle.
inline JitValue _ns_http_client(JitValue* a, int64_t n) {
  const char* ctx = "Http.client";
  std::string base_url(_ns_adapt::require_sv(a[0], "base_url"));
  culebra::http::HeaderList headers;
  JitValue h = _proc_adapt::at(a, n, 1);
  if (h.tag == TAG_OBJECT) {
    if (!_ns_env_object_pairs(reinterpret_cast<JitObject*>(h.data), headers)) {
      throw culebra::CulebraError("TypeError",
          std::format("{}: header values must be String", ctx), 0, 0);
    }
  } else if (h.tag != TAG_NIL) {
    throw culebra::CulebraError("TypeError",
        std::format("{}: headers must be an Object of String", ctx), 0, 0);
  }
  JitValue to = _proc_adapt::at(a, n, 2);
  if (to.tag != TAG_LONG && to.tag != TAG_NIL) {
    culebra::throw_type_mismatch("Long", _culebra_tag_name(to.tag), 0, 0);
  }
  int64_t timeout = (to.tag == TAG_LONG && to.data > 0) ? to.data : 0;
  JitValue fr = _proc_adapt::at(a, n, 3);
  bool follow;
  if (fr.tag == TAG_NIL) {
    follow = true;
  } else if (fr.tag == TAG_BOOL || fr.tag == TAG_LONG) {
    follow = fr.data != 0;
  } else if (fr.tag == TAG_FLOAT) {
    follow = _culebra_float_to_double(fr.data) != 0.0;
  } else {
    culebra::throw_type_mismatch("Bool, Long, or Float",
                                 _culebra_tag_name(fr.tag), 0, 0);
  }
  std::string err;
  int64_t id = culebra::http::http_client_open(base_url, std::move(headers),
                                               timeout, follow, err);
  if (id < 0) {
    throw culebra::CulebraError("HttpError",
        std::format("{}: {}", ctx, err), 0, 0);
  }
  return _culebra_http_build_client_handle(id);
}

// --- Http.server handle (JIT) ---
// Mirrors interp make_http_server_handle. v0 single-threaded: handlers run on
// the listen thread (where this runtime lives), so the trampoline calls the
// handler closure directly via _culebra_invoke1. The handle owns a +1 on the
// route handler closure for the server's lifetime (released when the server is
// closed, via the shared_ptr holder captured in the RouteHandler).

// Build the `req` Object from a neutral server request (mirrors interp
// http_request_to_object): method/path/body Strings + headers/query/params
// Objects of String.
inline JitObject* _jit_http_request_to_object(
    const culebra::http::ServerRequest& q) {
  auto* o = culebra_runtime_object_new();
  culebra_runtime_object_set(o, "method", false, TAG_STRING,
      reinterpret_cast<int64_t>(_culebra_heap_str(q.method)), 0, 0);
  culebra_runtime_object_set(o, "path", false, TAG_STRING,
      reinterpret_cast<int64_t>(_culebra_heap_str(q.path)), 0, 0);
  culebra_runtime_object_set(o, "body", false, TAG_STRING,
      reinterpret_cast<int64_t>(_culebra_heap_str(q.body)), 0, 0);
  // culebra_runtime_object_set is a codegen-facing ABI symbol and takes a
  // `const char*`, but only ever reads it as a string_view and interns the
  // name into the shape — so the NUL-terminated temporary below is enough.
  auto to_object = [](const culebra::http::HeaderViews& m) {
    auto* obj = culebra_runtime_object_new();
    for (const auto& [k, v] : m)
      culebra_runtime_object_set(obj, std::string(k).c_str(), false,
          TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(v)), 0, 0);
    return obj;
  };
  culebra_runtime_object_set(o, "headers", false, TAG_OBJECT,
      reinterpret_cast<int64_t>(to_object(q.headers)), 0, 0);
  culebra_runtime_object_set(o, "query", false, TAG_OBJECT,
      reinterpret_cast<int64_t>(to_object(q.params)), 0, 0);
  culebra_runtime_object_set(o, "params", false, TAG_OBJECT,
      reinterpret_cast<int64_t>(to_object(q.path_params)), 0, 0);
  return o;
}

// Apply a response Object's status/content_type/headers to `res` and return the
// resolved content_type (mirrors interp http_apply_response_meta). Shared by the
// buffered path (_jit_http_apply_response) and the streaming path
// (_jit_http_try_stream_response). A present, non-nil field of the wrong type is
// a TypeError — caught by the trampoline as a 500 — not a silently-defaulted
// value (interp reads these via to_long / to_string_view, same rejection).
inline std::string _jit_http_apply_response_meta(
    JitObject* o, culebra::http::ServerResponse& res) {
  int64_t status = 200;
  size_t si = o->find_slot("status");
  if (si != static_cast<size_t>(-1) && o->slots[si].value.tag != TAG_NIL) {
    JitValue v = o->slots[si].value;
    if (v.tag != TAG_LONG)
      culebra::throw_type_mismatch("Long", _culebra_tag_name(v.tag), 0, 0);
    status = v.data;
  }
  culebra::http::http_res_set_status(res, static_cast<int>(status));
  std::string content_type = "text/plain";
  size_t ci = o->find_slot("content_type");
  if (ci != static_cast<size_t>(-1) && o->slots[ci].value.tag != TAG_NIL) {
    JitValue v = o->slots[ci].value;
    if (v.tag != TAG_STRING && v.tag != TAG_STRINGVIEW)
      culebra::throw_type_mismatch("String", _culebra_tag_name(v.tag), 0, 0);
    content_type = std::string(_culebra_str_view(v.tag, v.data));
  }
  size_t hi = o->find_slot("headers");
  if (hi != static_cast<size_t>(-1) && o->slots[hi].value.tag == TAG_OBJECT) {
    auto* h = reinterpret_cast<JitObject*>(o->slots[hi].value.data);
    if (h->shape) {
      // Set incrementally and throw on the first non-String value, matching
      // interp's `res.set_header(k, val.to_string_view())` loop.
      for (size_t k = 0; k < h->shape->names.size(); k++) {
        JitValue v = h->slots[k].value;
        if (v.tag != TAG_STRING && v.tag != TAG_STRINGVIEW)
          culebra::throw_type_mismatch("String", _culebra_tag_name(v.tag), 0, 0);
        culebra::http::http_res_set_header(
            res, std::string(h->shape->names[k]),
            std::string(_culebra_str_view(v.tag, v.data)));
      }
    }
  }
  return content_type;
}

// Apply a handler's return value to the response (mirrors interp
// http_apply_response): String → 200 text/plain; Object → status/body/
// headers/content_type with defaults; nil → 200 empty; else TypeError.
inline void _jit_http_apply_response(JitValue ret,
                                     culebra::http::ServerResponse& res) {
  if (ret.tag == TAG_NIL) {
    culebra::http::http_res_set_status(res, 200);
    return;
  }
  if (ret.tag == TAG_STRING || ret.tag == TAG_STRINGVIEW) {
    culebra::http::http_res_set_status(res, 200);
    culebra::http::http_res_set_content(
        res, std::string(_culebra_str_view(ret.tag, ret.data)), "text/plain");
    return;
  }
  if (ret.tag == TAG_OBJECT) {
    auto* o = reinterpret_cast<JitObject*>(ret.data);
    std::string content_type = _jit_http_apply_response_meta(o, res);
    std::string body;
    size_t bi = o->find_slot("body");
    if (bi != static_cast<size_t>(-1) && o->slots[bi].value.tag != TAG_NIL) {
      JitValue v = o->slots[bi].value;
      if (v.tag != TAG_STRING && v.tag != TAG_STRINGVIEW)
        culebra::throw_type_mismatch("String", _culebra_tag_name(v.tag), 0, 0);
      body = std::string(_culebra_str_view(v.tag, v.data));
    }
    culebra::http::http_res_set_content(res, std::move(body), content_type);
    return;
  }
  throw culebra::CulebraError(
      "TypeError",
      "Http.server: handler must return a String, an Object, or nil", 0, 0);
}

// sink.write(chunk) for a streaming response — mirrors interp
// make_http_sink_handle's write. Returns Bool (false if the client has gone
// away or the sink has been invalidated). A `String` param rejects a StringView
// slice on every path, matching the interp/File handle-method convention.
CULEBRA_RT_INLINE void _jit_http_sink_write(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int64_t sid = _jit_handle_long(h, "_sink");
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "data");
  if (args[0].tag != TAG_STRING)
    _jit_file_param_type_error(self, "data", "String", 0);
  auto sv = _culebra_str_view(args[0].tag, args[0].data);
  bool ok = culebra::http::http_sink_write(sid, sv.data(), sv.size());
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_BOOL, ok ? 1 : 0}; return; }
}
// Build the `sink` handle (mirrors interp make_http_sink_handle): a thin
// non-sendable object over a sink id with a `write` method.
inline JitObject* _jit_make_http_sink_handle(int64_t sink_id) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_sink", JitValue{TAG_LONG, sink_id}, false);
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  static const JitParamMeta* write_meta =
      _jit_make_handle_meta({"data"}, {false});
  _jit_handle_bind_method(h, "write", _jit_http_sink_write, 1, write_meta);
  return h;
}

// Mirror interp http_try_stream_response: if `ret` is an Object carrying a
// `stream` Function, wire a chunked response (same status/content_type/headers
// fields, same order, same TypeError messages as _jit_http_apply_response) and
// return true; the caller then skips _jit_http_apply_response. body + stream is
// a TypeError. A mid-stream exception aborts the connection.
inline bool _jit_http_try_stream_response(JitValue ret,
                                          culebra::http::ServerResponse& res) {
  if (ret.tag != TAG_OBJECT) return false;
  auto* o = reinterpret_cast<JitObject*>(ret.data);
  size_t sti = o->find_slot("stream");
  if (sti == static_cast<size_t>(-1) || o->slots[sti].value.tag == TAG_NIL)
    return false;
  JitValue stream_v = o->slots[sti].value;
  if (stream_v.tag != TAG_FUNC)
    throw culebra::CulebraError(
        "TypeError", "Http.server: response stream must be a Function", 0, 0);
  size_t bi = o->find_slot("body");
  if (bi != static_cast<size_t>(-1) && o->slots[bi].value.tag != TAG_NIL)
    throw culebra::CulebraError(
        "TypeError", "Http.server: response cannot set both body and stream", 0,
        0);
  std::string content_type = _jit_http_apply_response_meta(o, res);
  // The provider runs after this returns (same worker thread, same response
  // write), and the closure lives only in C++ until then — so retain + pin it
  // (the mark-sweep backstop ignores refcount). A shared_ptr deleter unpins and
  // releases when httplib drops the provider, whether or not it ever ran.
  culebra_runtime_value_retain(TAG_FUNC, stream_v.data);
  _gc_heap().pin(reinterpret_cast<void*>(stream_v.data));
  std::shared_ptr<void> keep(reinterpret_cast<void*>(stream_v.data),
                             [](void* p) {
                               _gc_heap().unpin(p);
                               culebra_runtime_value_release(
                                   TAG_FUNC, reinterpret_cast<int64_t>(p));
                             });
  culebra::http::http_res_set_stream(
      res, content_type, [keep](int64_t sink_id) -> bool {
        auto* cls = reinterpret_cast<JitClosure*>(keep.get());
        try {
          JitValue sink{TAG_OBJECT, reinterpret_cast<int64_t>(
                                        _jit_make_http_sink_handle(sink_id))};
          JitValue r = _culebra_invoke1(cls, sink);  // consumes sink's +1
          _culebra_value_release_impl(r.tag, r.data);
          return true;
        } catch (...) {
          return false;
        }
      });
  return true;
}

// --- WebSocket handle (server-side connection or Http.ws client) ----------
// Mirrors interp make_http_ws_handle: send/receive/close/is_open + `iter` for
// for-in, and (client only) drop to free the owned connection.

// for-in pull: read the next message; close (or a stale id) ends iteration.
inline void _ws_messages_fast_fn(JitClosure* cls, JitValue, bool* done,
                                 int8_t* out_tag, int64_t* out_data) {
  int64_t id = cls->captures[0]->value.data;
  std::string out;
  if (culebra::http::ws_receive(id, out) == 0) { *done = true; return; }
  *done = false;
  *out_tag = TAG_STRING;
  *out_data = reinterpret_cast<int64_t>(_culebra_heap_str(out));
}

CULEBRA_RT_INLINE void _jit_ws_send(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                                        JitValue* args) {
  JitValue self{self_tag, self_data};
  auto* h = reinterpret_cast<JitObject*>(self.data);
  int64_t id = _jit_handle_long(h, "_ws");
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "data");
  if (args[0].tag != TAG_STRING)
    _jit_file_param_type_error(self, "data", "String", 0);
  auto sv = _culebra_str_view(args[0].tag, args[0].data);
  bool ok = culebra::http::ws_send(id, sv.data(), sv.size());
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_BOOL, ok ? 1 : 0}; return; }
}
CULEBRA_RT_INLINE void _jit_ws_receive(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                           JitValue*) {
  JitValue self{self_tag, self_data};
  int64_t id = _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_ws");
  std::string out;
  int got = culebra::http::ws_receive(id, out);
  culebra_runtime_value_release(self.tag, self.data);
  if (got == 0) { *__ret = {TAG_NIL, 0}; return; }  // nil = closed
  { *__ret = {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(out))}; return; }
}
CULEBRA_RT_INLINE void _jit_ws_close(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                         JitValue*) {
  JitValue self{self_tag, self_data};
  culebra::http::ws_close(
      _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_ws"));
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_ws_is_open(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                           JitValue*) {
  JitValue self{self_tag, self_data};
  bool open = culebra::http::ws_is_open(
      _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_ws"));
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_BOOL, open ? 1 : 0}; return; }
}
CULEBRA_RT_INLINE void _jit_ws_iter(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                        JitValue*) {
  JitValue self{self_tag, self_data};
  int64_t id = _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_ws");
  auto* id_cell = culebra_runtime_cell_new(TAG_LONG, id);
  auto* it = _iter_wrap_fast<&_ws_messages_fast_fn>({id_cell});
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
}
CULEBRA_RT_INLINE void _jit_ws_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                        JitValue*) {
  JitValue self{self_tag, self_data};
  // client drop (from the drop protocol — must NOT release self): close + free
  // the owned connection. A server ws has no drop (the trampoline frees it).
  culebra::http::ws_unregister(
      _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_ws"));
  { *__ret = {TAG_NIL, 0}; return; }
}

inline JitObject* _jit_make_http_ws_handle(int64_t ws_id, bool is_client) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_ws", JitValue{TAG_LONG, ws_id}, false);
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  static const JitParamMeta* send_meta =
      _jit_make_handle_meta({"data"}, {false});
  _jit_handle_bind_method(h, "send", _jit_ws_send, 1, send_meta);
  _jit_handle_bind_method(h, "receive", _jit_ws_receive, 0);
  _jit_handle_bind_method(h, "close", _jit_ws_close, 0);
  _jit_handle_bind_method(h, "is_open", _jit_ws_is_open, 0);
  _jit_handle_bind_method(h, "iter", _jit_ws_iter, 0);
  if (is_client) {
    _jit_handle_bind_method(h, "drop", _jit_ws_drop, 0);
    _jit_owned_bind_drop(h);
  }
  return h;
}

// Deferred route records per server id (registered at listen, when the worker
// count — and so whether handlers must be Sendable — is known). The handler is
// retained AND pinned for the record's lifetime: it lives only in this C++ map
// (and later httplib's route table), unreachable from any GC root, so a retain
// (RC) alone is not enough — the mark-sweep backstop sweeps unmarked objects
// regardless of refcount. Both are undone by the release at close. thread_local:
// the handle is non-sendable.
struct JitRouteRecord {
  std::string method;
  std::string pattern;
  JitValue handler;
};
inline thread_local std::unordered_map<int64_t, std::vector<JitRouteRecord>>
    g_jit_srv_routes;
// Per-worker deserialized handlers for a concurrent server (workers > 1): each
// worker thread rebuilds the handlers onto its own heap here.
inline thread_local std::vector<JitValue> g_jit_srv_w_handlers;

inline JitValue _jit_http_server_route(JitValue self, int64_t n, JitValue* args,
                                       const char* method) {
  int64_t id =
      _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id");
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "pattern");
  if (args[0].tag != TAG_STRING)
    _jit_file_param_type_error(self, "pattern", "String", 0);
  if (!_jit_file_arg_present(n, args, 1)) _jit_file_missing_arg(self, "handler");
  if (args[1].tag != TAG_FUNC)
    _jit_file_param_type_error(self, "handler", "Function", 1);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  culebra_runtime_value_retain(args[1].tag, args[1].data);
  _gc_heap().pin(reinterpret_cast<void*>(args[1].data));
  g_jit_srv_routes[id].push_back(
      {method, std::string(_culebra_str_view(args[0].tag, args[0].data)),
       args[1]});
  culebra_runtime_value_retain(self.tag, self.data);  // chainable return (+1)
  return self;
}

CULEBRA_RT_INLINE void _jit_http_server_get(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_server_route(self, n, args, "GET"); return; }
}
CULEBRA_RT_INLINE void _jit_http_server_post(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                 int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_server_route(self, n, args, "POST"); return; }
}
CULEBRA_RT_INLINE void _jit_http_server_put(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_server_route(self, n, args, "PUT"); return; }
}
CULEBRA_RT_INLINE void _jit_http_server_delete(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                   int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_server_route(self, n, args, "DELETE"); return; }
}
CULEBRA_RT_INLINE void _jit_http_server_patch(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                  int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_server_route(self, n, args, "PATCH"); return; }
}
CULEBRA_RT_INLINE void _jit_http_server_options(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                    int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_server_route(self, n, args, "OPTIONS"); return; }
}
CULEBRA_RT_INLINE void _jit_http_server_ws(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                               int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_server_route(self, n, args, "WS"); return; }
}

CULEBRA_RT_INLINE void _jit_http_server_static(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                   int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  int64_t id = _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id");
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "mount");
  if (args[0].tag != TAG_STRING)
    _jit_file_param_type_error(self, "mount", "String", 0);
  if (!_jit_file_arg_present(n, args, 1)) _jit_file_missing_arg(self, "dir");
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  *__ret = _jit_at_call_site([&] {
    std::string mount(_culebra_str_view(args[0].tag, args[0].data));
    std::string err;
    // dir is either a String path (live disk via mount point) or an
    // `Embed.dir(...)` handle object {__embed_dir__, name} (baked under
    // AOT, live disk otherwise). Mirrors the interp overload.
    // Mirror the interp message exactly (interp/JIT symmetry): a wrong type or
    // a forged Object that isn't a real Embed.dir handle both land here.
    auto bad_dir = [] {
      throw culebra::CulebraError(
          "TypeError",
          "server.static: dir must be a String path or Embed.dir(...)", 0, 0);
    };
    if (args[1].tag == TAG_OBJECT) {
      auto* o = reinterpret_cast<JitObject*>(args[1].data);
      // A real Embed.dir handle has both __embed_dir__ and name; a forged
      // lookalike missing either is rejected like any non-String (no OOB read).
      size_t ni = o->find_slot("name");
      if (o->find_slot("__embed_dir__") == static_cast<size_t>(-1) ||
          ni == static_cast<size_t>(-1))
        bad_dir();
      JitValue nv = o->slots[ni].value;
      culebra::http::http_server_serve_embed(
          id, mount, std::string(_culebra_str_view(nv.tag, nv.data)), err);
    } else if (args[1].tag == TAG_STRING) {
      culebra::http::http_server_static(
          id, mount, std::string(_culebra_str_view(args[1].tag, args[1].data)),
          err);
    } else {
      bad_dir();
    }
    if (!err.empty())
      throw culebra::CulebraError("HttpError",
                                  std::format("server.static: {}", err), 0, 0);
    culebra_runtime_value_retain(self.tag, self.data);  // chainable return (+1)
    return self;
  });
}

// Register the recorded routes and serve. Handlers always run on a worker pool
// (serialized once here, rebuilt per worker — so they must be Sendable); the
// accept loop never runs a handler. `async` selects background vs blocking serve.
inline JitValue _jit_http_server_do_serve(int64_t id, int64_t workers,
                                          bool async, const char* ctx) {
  std::string err;
  // Reject an unservable handle before installing any route trampoline, so a
  // rejected serve() leaves the handle exactly as it was.
  if (!culebra::http::http_server_serve_ready(id, err))
    throw culebra::CulebraError("HttpError", std::format("{}: {}", ctx, err), 0,
                                0);
  auto& recs = g_jit_srv_routes[id];
  {
    auto snodes = std::make_shared<std::vector<sendable::SendNode>>();
    for (const auto& rec : recs) {
      try {
        JitSerCtx sc;
        snodes->push_back(jit_serialize(rec.handler, sc));
      } catch (culebra::CulebraError& e) {
        throw culebra::CulebraError(
            "SendError",
            std::format("{}: handler for {} {} is not Sendable: {}", ctx,
                        rec.method, rec.pattern, e.what()),
            0, 0);
      }
    }
    for (size_t i = 0; i < recs.size(); i++) {
      size_t ri = i;
      std::string rerr;
      if (recs[i].method == "WS") {
        culebra::http::WsHandler wh =
            [ri](const culebra::http::ServerRequest& q,
                culebra::http::WebSocketRef* ws) {
              int64_t wsid = culebra::http::ws_register_server(ws);
              try {
                JitOwnedVal req(JitValue{
                    TAG_OBJECT,
                    reinterpret_cast<int64_t>(_jit_http_request_to_object(q))});
                JitOwnedVal wsh(JitValue{
                    TAG_OBJECT, reinterpret_cast<int64_t>(
                                    _jit_make_http_ws_handle(wsid, false))});
                auto* cb = reinterpret_cast<JitClosure*>(
                    g_jit_srv_w_handlers[ri].data);
                // The invoke consumes both +1s; owning them until then covers
                // a throw while the second one is still being built.
                JitOwnedVal r(_culebra_invoke2(cb, req.consume(),
                                               wsh.consume()));
              } catch (...) {
              }
              culebra::http::ws_unregister(wsid);
            };
        culebra::http::http_server_ws(id, recs[i].pattern, std::move(wh), rerr);
      } else {
        culebra::http::RouteHandler rh =
            [ri](const culebra::http::ServerRequest& q,
                culebra::http::ServerResponse& res) {
              try {
                JitValue req{TAG_OBJECT, reinterpret_cast<int64_t>(
                                             _jit_http_request_to_object(q))};
                auto* cb = reinterpret_cast<JitClosure*>(
                    g_jit_srv_w_handlers[ri].data);
                // Owned: applying the response raises a TypeError on a
                // malformed one (bad `status`, `stream` plus `body`, …), and
                // the catch below turns that into a 500 — a tail release would
                // never run, stranding the handler's result and its captures.
                JitOwnedVal ret(_culebra_invoke1(cb, req));
                if (!_jit_http_try_stream_response(ret.borrow(), res))
                  _jit_http_apply_response(ret.borrow(), res);
              } catch (const std::exception& e) {
                culebra::http::http_res_set_status(res, 500);
                culebra::http::http_res_set_content(res, e.what(),
                                                    "text/plain");
              }
            };
        culebra::http::http_server_route(id, recs[i].method, recs[i].pattern,
                                         std::move(rh), rerr);
      }
      if (!rerr.empty())
        throw culebra::CulebraError("HttpError",
                                    std::format("{}: {}", ctx, rerr), 0, 0);
    }
    auto setup = [snodes]() {
      g_jit_srv_w_handlers.clear();
      g_jit_srv_w_handlers.reserve(snodes->size());
      for (const auto& sn : *snodes) {
        JitDeCtx dc;
        JitValue h = jit_deserialize(sn, dc);  // +1
        // Held only in this C++ vector → pin so the GC backstop won't sweep it.
        _gc_heap().pin(reinterpret_cast<void*>(h.data));
        g_jit_srv_w_handlers.push_back(h);
      }
    };
    auto teardown = []() {
      for (auto& h : g_jit_srv_w_handlers)
        culebra_runtime_value_release(h.tag, h.data);
      g_jit_srv_w_handlers.clear();
    };
    bool ok = async ? culebra::http::http_server_serve_async(
                          id, static_cast<int>(workers), setup, teardown, err)
                    : culebra::http::http_server_serve(
                          id, static_cast<int>(workers), setup, teardown, err);
    if (!ok)
      throw culebra::CulebraError("HttpError", std::format("{}: {}", ctx, err),
                                  0, 0);
    return {TAG_NIL, 0};
  }
}

CULEBRA_RT_INLINE int64_t _jit_http_server_id(JitValue self) {
  return _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id");
}

// args[0] = port, args[1] = host. Shared by bind and listen. The caller has
// already checked that port is present (that check must run before the self
// guard exists — _jit_file_missing_arg releases self itself).
inline int64_t _jit_http_server_bind_args(int64_t id, int64_t n, JitValue* args,
                                          const char* ctx) {
  if (args[0].tag != TAG_LONG)
    culebra::throw_type_mismatch("Long", _culebra_tag_name(args[0].tag), 0, 0);
  std::string host = "0.0.0.0";
  if (_jit_file_arg_present(n, args, 1) && args[1].tag != TAG_NIL)
    host = std::string(_culebra_str_view(args[1].tag, args[1].data));
  std::string err;
  int bound = culebra::http::http_server_bind(
      id, host, static_cast<int>(args[0].data), err);
  if (bound < 0)
    throw culebra::CulebraError("HttpError", std::format("{}: {}", ctx, err), 0,
                                0);
  return bound;
}

// `workers` at args[idx]; absent / nil = 0 = the CPU-scaled pool.
inline int64_t _jit_http_server_workers_arg(int64_t n, JitValue* args,
                                            int64_t idx) {
  if (!_jit_file_arg_present(n, args, idx) || args[idx].tag == TAG_NIL) return 0;
  if (args[idx].tag != TAG_LONG)
    culebra::throw_type_mismatch("Long", _culebra_tag_name(args[idx].tag), 0,
                                 0);
  return args[idx].data;
}

CULEBRA_RT_INLINE void _jit_http_server_bind(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                 int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "port");
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  *__ret = _jit_at_call_site([&] {
    return JitValue{TAG_LONG,
                    _jit_http_server_bind_args(_jit_http_server_id(self), n,
                                               args, "server.bind")};
  });
}

// Shared body for serve (async=false, blocks) and serve_async (returns).
inline JitValue _jit_http_server_serve_impl(JitValue self, int64_t n,
                                            JitValue* args, bool async) {
  int64_t id = _jit_http_server_id(self);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  return _jit_at_call_site([&] {
    return _jit_http_server_do_serve(
        id, _jit_http_server_workers_arg(n, args, 0), async,
        async ? "server.serve_async" : "server.serve");
  });
}
CULEBRA_RT_INLINE void _jit_http_server_serve(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                  int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_server_serve_impl(self, n, args, /*async=*/false); return; }
}
CULEBRA_RT_INLINE void _jit_http_server_serve_async(JitValue* __ret, JitClosure*,
                                                        int8_t self_tag, int64_t self_data,
                                                        int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_server_serve_impl(self, n, args, /*async=*/true); return; }
}

// listen = bind + serve. Blocking returns nil (it only returns once stopped);
// async returns the bound port — the only way to learn a port-0 one.
inline JitValue _jit_http_server_listen_impl(JitValue self, int64_t n,
                                             JitValue* args, bool async) {
  int64_t id = _jit_http_server_id(self);
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "port");
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  return _jit_at_call_site([&] {
    const char* ctx = async ? "server.listen_async" : "server.listen";
    int64_t bound = _jit_http_server_bind_args(id, n, args, ctx);
    int64_t workers = _jit_http_server_workers_arg(n, args, 2);
    JitValue r = _jit_http_server_do_serve(id, workers, async, ctx);
    return async ? JitValue{TAG_LONG, bound} : r;
  });
}
CULEBRA_RT_INLINE void _jit_http_server_listen(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                   int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_server_listen_impl(self, n, args, /*async=*/false); return; }
}
CULEBRA_RT_INLINE void _jit_http_server_listen_async(JitValue* __ret, JitClosure*,
                                                         int8_t self_tag, int64_t self_data,
                                                         int64_t n,
                                                         JitValue* args) {
  JitValue self{self_tag, self_data};
  { *__ret = _jit_http_server_listen_impl(self, n, args, /*async=*/true); return; }
}
CULEBRA_RT_INLINE void _jit_http_server_stop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                 int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  culebra::http::http_server_stop(
      _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id"));
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_NIL, 0}; return; }
}

// Release the recorded handlers (their retain + the registry entry; the pin is
// dropped when RC frees them), drop the records, and free the server.
inline void _jit_http_server_clear_routes(int64_t id) {
  auto it = g_jit_srv_routes.find(id);
  if (it == g_jit_srv_routes.end()) return;
  for (auto& rec : it->second)
    culebra_runtime_value_release(rec.handler.tag, rec.handler.data);
  g_jit_srv_routes.erase(it);
}
CULEBRA_RT_INLINE void _jit_http_server_close(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                  int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  int64_t id = _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id");
  _jit_http_server_clear_routes(id);
  culebra::http::http_server_close(id);
  culebra_runtime_value_release(self.tag, self.data);
  { *__ret = {TAG_NIL, 0}; return; }
}
CULEBRA_RT_INLINE void _jit_http_server_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                 int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // drop runs from the destructor's drop protocol — must NOT release self.
  int64_t id = _jit_handle_long(reinterpret_cast<JitObject*>(self.data), "_id");
  _jit_http_server_clear_routes(id);
  culebra::http::http_server_close(id);
  { *__ret = {TAG_NIL, 0}; return; }
}

CULEBRA_RT_INLINE JitValue _culebra_http_build_server_handle(int64_t id) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_id", JitValue{TAG_LONG, id}, false);
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  static const JitParamMeta* route_meta =
      _jit_make_handle_meta({"pattern", "handler"}, {false, false});
  static const JitParamMeta* static_meta =
      _jit_make_handle_meta({"mount", "dir"}, {false, false});
  static const JitParamMeta* bind_meta =
      _jit_make_handle_meta({"port", "host"}, {false, true});
  static const JitParamMeta* serve_meta =
      _jit_make_handle_meta({"workers"}, {true});
  static const JitParamMeta* listen_meta =
      _jit_make_handle_meta({"port", "host", "workers"}, {false, true, true});
  _jit_handle_bind_method(h, "get", _jit_http_server_get, 2, route_meta);
  _jit_handle_bind_method(h, "post", _jit_http_server_post, 2, route_meta);
  _jit_handle_bind_method(h, "put", _jit_http_server_put, 2, route_meta);
  _jit_handle_bind_method(h, "delete", _jit_http_server_delete, 2, route_meta);
  _jit_handle_bind_method(h, "patch", _jit_http_server_patch, 2, route_meta);
  _jit_handle_bind_method(h, "options", _jit_http_server_options, 2,
                          route_meta);
  _jit_handle_bind_method(h, "ws", _jit_http_server_ws, 2, route_meta);
  _jit_handle_bind_method(h, "static", _jit_http_server_static, 2, static_meta);
  _jit_handle_bind_method(h, "bind", _jit_http_server_bind, 1, bind_meta);
  _jit_handle_bind_method(h, "serve", _jit_http_server_serve, 0, serve_meta);
  _jit_handle_bind_method(h, "serve_async", _jit_http_server_serve_async, 0,
                          serve_meta);
  _jit_handle_bind_method(h, "listen", _jit_http_server_listen, 1, listen_meta);
  _jit_handle_bind_method(h, "listen_async", _jit_http_server_listen_async, 1,
                          listen_meta);
  _jit_handle_bind_method(h, "stop", _jit_http_server_stop, 0);
  _jit_handle_bind_method(h, "close", _jit_http_server_close, 0);
  _jit_handle_bind_method(h, "drop", _jit_http_server_drop, 0);
  _jit_owned_bind_drop(h);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// Http.server() → server handle.
inline JitValue _ns_http_server(JitValue*, int64_t) {
  return _culebra_http_build_server_handle(culebra::http::http_server_open());
}

// Http.ws(url) → connected WebSocket client handle (HttpError on failure).
inline JitValue _ns_http_ws(JitValue* a, int64_t) {
  std::string url(_ns_adapt::require_sv(a[0], "url"));
  std::string err;
  int64_t id = culebra::http::ws_client_open(url, err);
  if (id < 0) throw culebra::CulebraError("HttpError", err, 0, 0);
  return {TAG_OBJECT,
          reinterpret_cast<int64_t>(_jit_make_http_ws_handle(id, true))};
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
// compile_ns_call fast path); the adapter builds everything directly.
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
  int64_t id = culebra::freeze_shared_val(std::move(root));
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
  int64_t count = a[0].data;
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
  int64_t id = culebra::make_shared_buffer(*layout, std::string(cname),
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
  int64_t count = a[1].data;
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
  int64_t id = culebra::make_shared_buffer_file(*layout, std::string(cname),
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
  int64_t count = a[0].data;
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
  int64_t id = culebra::make_shared_buffer_shared(*layout, std::string(cname),
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
  int64_t id = culebra::make_shared_buffer_from_share_env(*layout,
                                                       std::string(cname), name);
  auto core = culebra::lookup_shared_buffer(id);
  return _jit_make_shared_buffer_handle(
      id, core ? static_cast<int64_t>(core->count) : 0);
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
  int64_t limit = (n >= 3 && a[2].tag == TAG_LONG) ? a[2].data : 0;
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
  std::vector<int> fds;
  bool has_share = _proc_adapt::share_slot(_proc_adapt::at(a, n, 4),
                                           "Proc.spawn", over, fds);
  if (has_share) env = &over;
  return _culebra_proc_spawn_build(a[0].tag, a[0].data, cwd, env, stdin_data,
                                   0, 0, fds.empty() ? nullptr : &fds);
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
// JSON.parse(s, lines=false, number_mode="auto", jsonc=false): same
// variable-prefix slab.
inline JitValue _ns_json_parse(JitValue* a, int64_t n) {
  _ns_adapt::require_sv(a[0], "s");  // s: String
  int8_t lines = (n > 1) ? (_ns_adapt::require_bool(a[1], "lines") ? 1 : 0) : 0;
  std::string mode =
      (n > 2) ? std::string(_ns_adapt::require_sv(a[2], "number_mode")) : "auto";
  int8_t jsonc = (n > 3) ? (_ns_adapt::require_bool(a[3], "jsonc") ? 1 : 0) : 0;
  if (mode != "auto" && mode != "float") {
    // interp validates this in the method body (a ValueError, not a typed-param
    // TypeError); 0/0 backfills to the call site like the interp's position.
    culebra::throw_runtime_error_at(
        "ValueError", "JSON.parse: number_mode must be 'auto' or 'float'", 0, 0);
  }
  return culebra_runtime_json_parse(_ns_adapt::take_str(a[0]), mode.c_str(),
                                     lines, jsonc);
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
// deflate(data, level=-1): level is optional, matching FS.remove's
// recursive=false shape — the kwarg resolver fills a[1] from either a
// positional or a `level:` call before this adapter ever runs, so `n > 1`
// alone distinguishes "omitted" from "given" and the type/arity checks it
// implies are already done at the canonical NsParamMeta layer.
inline JitValue _ns_compress_deflate(JitValue* a, int64_t n) {
  int64_t level = n > 1 ? _ns_adapt::take_long(a[1]) : -1;
  auto r = culebra::compress::deflate_zlib(_ns_adapt::require_sv(a[0], "data"),
                                           static_cast<int>(level));
  if (!r.error.empty()) {
    throw culebra::CulebraError("ValueError", "Compress.deflate: " + r.error,
                                0, 0);
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

// CSV.{parse,stringify}: shared RFC-4180-ish logic via csv.h. parse returns
// Array<Array<String>>; stringify takes an Array of Array rows and renders
// each field via value_to_display (the to_string conversion, symmetric with
// interp). require_sv keeps parse binary-safe. Slow-path only.
// `delimiter:` is the optional second slot the kwarg-slab binder fills (after
// the required positional), defaulting to ',' — matching interp's csv_delim_char.
inline char _csv_delim(JitValue* a, int64_t n) {
  if (n < 2) return ',';
  auto sv = _ns_adapt::require_sv(a[1], "delimiter");
  return sv.empty() ? ',' : sv[0];
}
// Map a coerced CSV cell to a JitValue (JIT side of the neutral result).
inline JitValue _csv_coerced_to_jit(const culebra::csv::CoercedCell& cc) {
  using CT = culebra::csv::ColType;
  switch (cc.kind) {
    case CT::Long:  return {TAG_LONG, cc.long_val};
    case CT::Float: return jit_float(cc.float_val);
    case CT::Bool:  return {TAG_BOOL, cc.bool_val ? 1 : 0};
    case CT::String:
      break;
  }
  return {TAG_STRING,
          reinterpret_cast<int64_t>(_culebra_heap_str(std::string(cc.str_val)))};
}
inline JitValue _ns_csv_parse(JitValue* a, int64_t n) {
  namespace ccsv = culebra::csv;
  auto rows =
      ccsv::parse(_ns_adapt::require_sv(a[0], "text"), _csv_delim(a, n));
  // header (slot 2): Bool, default false (nil/absent → false). types (slot 3):
  // an Object of String type names, or nil. Layout derives from interp params.
  JitValue hv = _proc_adapt::at(a, n, 2);
  bool header;
  if (hv.tag == TAG_NIL) header = false;  // absent (positional slow path) → default
  else if (hv.tag == TAG_BOOL) header = hv.data != 0;
  else {
    // Match interp's typed-param binder wording (the kwarg resolver does no
    // per-param type check, so this adapter is where `header:` is validated).
    culebra::throw_runtime_error_at(
        "TypeError", "type error: parameter 'header' expects Bool", 0, 0);
  }
  JitValue tv = _proc_adapt::at(a, n, 3);
  bool has_types = tv.tag != TAG_NIL;
  if (has_types && !header) {
    throw culebra::CulebraError("TypeError",
        "CSV.parse: types requires header: true", 0, 0);
  }
  if (!header) {
    auto* outer = culebra_runtime_array_new();
    for (auto& row : rows) {
      auto* inner = culebra_runtime_array_new();
      for (auto& f : row)
        culebra_runtime_array_push(
            inner, TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(f)));
      culebra_runtime_array_push(outer, TAG_ARRAY,
                                 reinterpret_cast<int64_t>(inner));
    }
    return _ns_adapt::v_array(outer);
  }
  auto* outer = culebra_runtime_array_new();
  if (rows.empty()) return _ns_adapt::v_array(outer);  // no header row → empty
  const auto& head = rows[0];
  std::vector<std::pair<std::string, std::string>> tpairs;
  if (has_types) {
    if (tv.tag != TAG_OBJECT) {
      throw culebra::CulebraError("TypeError",
          "CSV.parse: types must be an Object of String", 0, 0);
    }
    if (!_ns_env_object_pairs(reinterpret_cast<JitObject*>(tv.data), tpairs)) {
      throw culebra::CulebraError("TypeError",
          "CSV.parse: type values must be String", 0, 0);
    }
  }
  std::vector<ccsv::ColType> cts;
  std::string err;
  if (!ccsv::resolve_col_types(head, tpairs, cts, err)) {
    throw culebra::CulebraError("ValueError", "CSV.parse: " + err, 0, 0);
  }
  for (size_t r = 1; r < rows.size(); r++) {
    const auto& row = rows[r];
    if (row.size() != head.size()) {
      throw culebra::CulebraError("ValueError",
          std::format("CSV.parse: row {} has {} fields, but the header has {}",
                      r + 1, row.size(), head.size()), 0, 0);
    }
    auto* obj = culebra_runtime_object_new();
    for (size_t c = 0; c < head.size(); c++) {
      ccsv::CoercedCell cc;
      if (!ccsv::coerce_cell(row[c], cts[c], cc)) {
        throw culebra::CulebraError("ValueError",
            std::format("CSV.parse: row {}, column '{}': expected {}, got '{}'",
                        r + 1, head[c], ccsv::col_type_name(cts[c]), row[c]),
            0, 0);
      }
      JitValue v = _csv_coerced_to_jit(cc);
      culebra_runtime_object_set(obj, head[c].c_str(), false, v.tag, v.data, 0,
                                 0);
    }
    culebra_runtime_array_push(outer, TAG_OBJECT,
                               reinterpret_cast<int64_t>(obj));
  }
  return _ns_adapt::v_array(outer);
}
inline JitValue _ns_csv_stringify(JitValue* a, int64_t n) {
  // a[0] is Array (the binder enforces the declared type at the arg position).
  auto* rows = reinterpret_cast<JitArray*>(a[0].data);
  std::vector<std::vector<std::string>> grid;
  for (size_t i = 0; i < rows->size; i++) {
    const JitValue& rv = rows->items[i];
    if (rv.tag != TAG_ARRAY) culebra::throw_type_error_at(0, 0);
    auto* r = reinterpret_cast<JitArray*>(rv.data);
    std::vector<std::string> out;
    for (size_t j = 0; j < r->size; j++) {
      const JitValue& fv = r->items[j];
      // A String field keeps its exact bytes (length-aware, NUL-safe — matching
      // interp's str_display); value_to_display returns a C string that would
      // truncate at an embedded NUL. Non-string scalars display NUL-free.
      if (fv.tag == TAG_STRING || fv.tag == TAG_STRINGVIEW)
        out.emplace_back(_culebra_str_view(fv.tag, fv.data));
      else
        out.emplace_back(culebra_runtime_value_to_display(fv.tag, fv.data));
    }
    grid.push_back(std::move(out));
  }
  return _ns_adapt::v_string(
      _culebra_heap_str(culebra::csv::stringify(grid, _csv_delim(a, n))));
}

// TOML.{parse,stringify}: shared grammar/serialization via toml.h. parse
// returns the Object/Array/scalar tree (date-times as raw Strings); stringify
// takes an Object and renders it with `[section]` expansion. Mirrors the
// interp converters so the backends agree byte-for-byte. Slow-path only;
// `sort_keys:` is the optional second slot the kwarg-slab binder fills.

// Build a JitValue tree from a neutral toml::Node (each heap object is +1
// owned by the caller, handed to the user binding).
inline JitValue _toml_node_to_jit(const culebra::toml::Node& n) {
  using K = culebra::toml::Kind;
  switch (n.kind) {
    case K::String:
      return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(n.s))};
    case K::Int:   return {TAG_LONG, n.i};
    case K::Float: return {TAG_FLOAT, _culebra_double_to_bits(n.f)};
    case K::Bool:  return {TAG_BOOL, n.b ? 1 : 0};
    case K::Array: {
      auto* arr = culebra_runtime_array_new();
      for (const auto& e : n.elems) {
        auto v = _toml_node_to_jit(e);
        culebra_runtime_array_push(arr, v.tag, v.data);
      }
      return {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
    }
    case K::Table: {
      auto* obj = culebra_runtime_object_new();
      for (const auto& kv : n.items) {
        auto v = _toml_node_to_jit(kv.second);
        culebra_runtime_object_set(obj, kv.first.c_str(), false, v.tag, v.data,
                                   0, 0);
      }
      return {TAG_OBJECT, reinterpret_cast<int64_t>(obj)};
    }
  }
  return {TAG_NIL, 0};
}

inline void _toml_check_stringify_depth(int64_t depth, int64_t line,
                                        int64_t col) {
  if (depth >= culebra::toml::kTomlDepthLimit) {
    throw culebra::CulebraError("ValueError",
        "TOML.stringify: " + culebra::toml::depth_message(), line, col);
  }
}

// Convert a JitValue into a neutral toml::Node for serialization. Objects
// become tables (non-String keys rejected, like JSON); Array/Tuple/Set become
// arrays; scalars map across; everything else is a TypeError. `depth` guards
// the recursion: a loop can build a value deeper than any parse.
inline culebra::toml::Node _jit_to_toml_node(int8_t tag, int64_t data,
                                             int64_t line, int64_t col,
                                             int64_t depth = 0) {
  using N = culebra::toml::Node;
  switch (tag) {
    case TAG_BOOL:  return N::boolean(data != 0);
    case TAG_LONG:  return N::integer(data);
    case TAG_FLOAT: return N::floating(_culebra_float_to_double(data));
    case TAG_STRING:
    case TAG_STRINGVIEW:
      return N::string(std::string(_culebra_str_view(tag, data)));
    case TAG_ARRAY:
    case TAG_TUPLE: {
      _toml_check_stringify_depth(depth, line, col);
      auto* arr = reinterpret_cast<JitArray*>(data);
      N a = N::array();
      for (size_t i = 0; i < arr->size; i++)
        a.elems.push_back(_jit_to_toml_node(arr->items[i].tag,
                                            arr->items[i].data, line, col,
                                            depth + 1));
      return a;
    }
    case TAG_SET: {
      _toml_check_stringify_depth(depth, line, col);
      auto* set = reinterpret_cast<JitSet*>(data);
      N a = N::array();
      for (const auto& m : set->members)
        a.elems.push_back(_jit_to_toml_node(m.tag, m.data, line, col,
                                            depth + 1));
      return a;
    }
    case TAG_OBJECT: {
      _toml_check_stringify_depth(depth, line, col);
      auto* obj = reinterpret_cast<JitObject*>(data);
      if (obj->non_string_props && !obj->non_string_props->empty()) {
        throw culebra::CulebraError("TypeError",
            "TOML.stringify: Object has non-String keys", line, col);
      }
      N t = N::table();
      if (obj->shape) {
        for (size_t i = 0; i < obj->shape->names.size(); i++)
          t.set(std::string(obj->shape->names[i]),
                _jit_to_toml_node(obj->slots[i].value.tag,
                                  obj->slots[i].value.data, line, col,
                                  depth + 1));
      }
      return t;
    }
  }
  throw culebra::CulebraError("TypeError", std::format(
      "TOML.stringify: cannot serialize {}", _culebra_tag_name(tag)), line, col);
}

inline JitValue _ns_toml_parse(JitValue* a, int64_t /*n*/) {
  try {
    auto root = culebra::toml::parse(_ns_adapt::require_sv(a[0], "text"));
    return _toml_node_to_jit(root);
  } catch (const culebra::toml::ParseError& e) {
    throw culebra::CulebraError("ValueError",
        std::format("TOML.parse: {}", e.message), e.line, e.col);
  }
}

inline JitValue _ns_toml_stringify(JitValue* a, int64_t n) {
  // a[0] is Object (the binder enforces the declared type at the arg position).
  // a[1] is the `sort_keys` Bool slot, default false when the slab left it off.
  bool sort_keys = (n >= 2) && (a[1].data != 0);
  auto root = _jit_to_toml_node(a[0].tag, a[0].data, 0, 0);
  return _ns_adapt::v_string(
      _culebra_heap_str(culebra::toml::stringify(root, sort_keys)));
}

// Env.{parse,load}: dotenv parsing via env.h (shared with interp). Both return
// an Object of String values; `load` also reads a file and sets each entry into
// the process environment (overwriting only when `override: true`).
inline JitObject* _env_build_object(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  auto* o = culebra_runtime_object_new();
  for (const auto& [k, v] : pairs)
    culebra_runtime_object_set(o, k.c_str(), /*mut=*/false, TAG_STRING,
                               reinterpret_cast<int64_t>(_culebra_heap_str(v)),
                               0, 0);
  return o;
}
inline JitValue _ns_env_parse(JitValue* a, int64_t) {
  auto pairs = culebra::env::parse(_ns_adapt::require_sv(a[0], "text"));
  return _ns_adapt::v_object(_env_build_object(pairs));
}
inline JitValue _ns_env_load(JitValue* a, int64_t n) {
  // Kwarg slab: a[0]=path (".env" default), a[1]=override (false default).
  // require_bool enforces the declared Bool type so a non-Bool `override:`
  // raises the same TypeError as the interp binder (not a silent coerce).
  std::string path = n > 0 ? _ns_adapt::take_str(a[0]) : ".env";
  bool overwrite = n > 1 && _ns_adapt::require_bool(a[1], "override");
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs)
    throw culebra::CulebraError(
        "IOError", std::format("Env.load: cannot open '{}'", path), 0, 0);
  std::string content((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  auto pairs = culebra::env::parse(content);
  for (const auto& [k, v] : pairs)
    os_setenv(k.c_str(), v.c_str(), overwrite ? 1 : 0);
  return _ns_adapt::v_object(_env_build_object(pairs));
}

// String.from_code_point(cp): shared with interp via string_from_code_point
// (shared.h) — raises ValueError for values above U+10FFFF or in the
// surrogate range, same boundary the `\u`/`\U` literal escapes enforce.
inline JitValue _ns_string_from_code_point(JitValue* a, int64_t) {
  return _ns_adapt::v_string(
      _culebra_heap_str(culebra::string_from_code_point(_ns_adapt::take_long(a[0]))));
}

// String.from_bytes(bytes): the inverse of `.bytes()`. Interp twin walks
// `bytes` the same way (element-by-element, TypeError on a non-Long,
// append_checked_byte from shared.h for the ValueError gate) so both
// backends agree on which element fails and with what message.
inline JitValue _ns_string_from_bytes(JitValue* a, int64_t) {
  ::JitArray* arr = _ns_adapt::take_array(a[0]);
  std::string out;
  out.reserve(arr->size);
  for (size_t i = 0; i < arr->size; i++) {
    if (arr->items[i].tag != ::TAG_LONG) throw_type_error_at(0, 0);
    culebra::append_checked_byte(out, _ns_adapt::take_long(arr->items[i]));
  }
  return _ns_adapt::v_string(_culebra_heap_str(out));
}

// String.from_code_points(cps): the plural inverse of `.code_points()`.
// Same element-by-element walk as `from_bytes`, gated by
// append_checked_code_point instead — the same scalar-value check
// `from_code_point` uses, so `from_code_points([cp]) == from_code_point(cp)`.
inline JitValue _ns_string_from_code_points(JitValue* a, int64_t) {
  ::JitArray* arr = _ns_adapt::take_array(a[0]);
  std::string out;
  out.reserve(arr->size);
  for (size_t i = 0; i < arr->size; i++) {
    if (arr->items[i].tag != ::TAG_LONG) throw_type_error_at(0, 0);
    culebra::append_checked_code_point(out, _ns_adapt::take_long(arr->items[i]));
  }
  return _ns_adapt::v_string(_culebra_heap_str(out));
}

// UUID.{v4,v7}: canonical UUID strings via uuid.h (shared entropy/format).
inline JitValue _ns_uuid_v4(JitValue*, int64_t) {
  return _ns_adapt::v_string(_culebra_heap_str(culebra::uuid::v4()));
}
inline JitValue _ns_uuid_v7(JitValue*, int64_t) {
  return _ns_adapt::v_string(_culebra_heap_str(culebra::uuid::v7()));
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
inline JitValue _ns_tensor_concat(JitValue* a, int64_t) {
  return _ns_adapt::v_tensor(
      culebra_runtime_tensor_concat(_ns_adapt::take_array(a[0]), 0, 0));
}
inline JitValue _ns_tensor_from_csv(JitValue* a, int64_t) {
  return _ns_adapt::v_tensor(
      culebra_runtime_tensor_from_csv(_ns_adapt::take_str(a[0])));
}
inline JitValue _ns_tensor_no_grad(JitValue* a, int64_t) {
  // The "Function" gate also admits a structural callable (a __call__
  // object), but the closure-invoke ABI only handles real closures.
  // interp's _invoke_callback rejects a non-closure via to_function();
  // mirror that exact wording (call-site position is filled by the
  // dispatch wrapper) instead of reinterpret_cast'ing into a crash.
  if (a[0].tag != TAG_FUNC) {
    throw culebra::CulebraError(
        "TypeError",
        culebra::type_mismatch_message("Function",
                                       culebra_runtime_type_of(a[0].tag)));
  }
  return culebra_runtime_tensor_no_grad(
      reinterpret_cast<JitClosure*>(a[0].data));
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

// Device selection. The setters call tl through backend-free wrappers; the
// query is choked (weak stub -> false) so a no-tensor binary links no Metal
// even though the dispatch table retains all these adapters.
inline JitValue _ns_tensor_use_cpu(JitValue*, int64_t) {
  culebra::tensor_use_cpu();
  return _ns_adapt::v_nil();
}
inline JitValue _ns_tensor_use_gpu(JitValue*, int64_t) {
  culebra::tensor_use_gpu();
  return _ns_adapt::v_nil();
}
inline JitValue _ns_tensor_use_auto(JitValue*, int64_t) {
  culebra::tensor_use_auto();
  return _ns_adapt::v_nil();
}
inline JitValue _ns_tensor_gpu_available(JitValue*, int64_t) {
  return _ns_adapt::v_bool(culebra::tensor_gpu_available());
}
inline JitValue _ns_tensor_device(JitValue*, int64_t) {
  // tensor_device returns a static literal; intern it so the surfaced
  // TAG_STRING carries a length header like any other String.
  return _ns_adapt::v_string(_intern_str(culebra::tensor_device()));
}

// --- The dispatch table ---

//===-- Regex: the `Regex` namespace functions. Like GC.stat, no fast-path
// branch or runtime helper is needed — these slow-path adapters do the work
// and build the JitObject/JitArray results directly. All take (pattern,
// subject, ...); flags are inline ((?i)/(?m)/(?s)). A Match is a data object
// { value, start, end, groups:[Group|nil], named:{name:Group} }; no-match nil.
//===------------------------------------------------------------------------//
inline std::shared_ptr<reg::Regex> _jit_regex_compile(std::string_view pat) {
  // Stateless cache keyed by pattern (own thread-local; see the /simplify note
  // about sharing with stdlib_interp's regex_compile_cached).
  static thread_local std::unordered_map<std::string,
                                         std::shared_ptr<reg::Regex>>
      cache;
  std::string p(pat);
  auto it = cache.find(p);
  if (it != cache.end()) return it->second;
  std::shared_ptr<reg::Regex> re;
  try {
    re = std::make_shared<reg::Regex>(p);
  } catch (const reg::RegexError& e) {
    throw culebra::CulebraError("RegexError",
                                std::format("Regex: {}", e.what()), 0, 0);
  }
  if (cache.size() > 256) cache.clear();
  cache.emplace(std::move(p), re);
  return re;
}

// `offset` shifts byte spans to absolute positions (find_from searches a suffix);
// the engine's match views are immutable, so the shift is applied at build time.
inline JitValue _jit_regex_group(const reg::Match& c, size_t offset = 0) {
  if (!c.matched()) return _ns_adapt::v_nil();
  auto* g = culebra_runtime_object_new();
  culebra_runtime_object_set(
      g, "value", false, TAG_STRING,
      reinterpret_cast<int64_t>(_culebra_heap_str(c.str())), 0, 0);
  culebra_runtime_object_set(g, "start", false, TAG_LONG,
                             static_cast<int64_t>(c.begin() + offset), 0, 0);
  culebra_runtime_object_set(g, "end", false, TAG_LONG,
                             static_cast<int64_t>(c.end() + offset), 0, 0);
  return _ns_adapt::v_object(g);
}

// Templated over match-like type (`reg::MatchResult` owning / `reg::Match` view)
// so search/match and find_all share one builder. `named` is the Regex's
// name->index map; `offset` the absolute shift. Mirrors stdlib_interp's twin.
template <typename MatchT>
inline JitValue _jit_regex_match(
    const MatchT& m, const std::unordered_map<std::string, int>& named,
    size_t offset = 0) {
  auto* o = culebra_runtime_object_new();
  o->is_match = true;  // route `m[i]` / `m["name"]` to capture groups
  culebra_runtime_object_set(
      o, "value", false, TAG_STRING,
      reinterpret_cast<int64_t>(_culebra_heap_str(m.str())), 0, 0);
  culebra_runtime_object_set(o, "start", false, TAG_LONG,
                             static_cast<int64_t>(m.begin() + offset), 0, 0);
  culebra_runtime_object_set(o, "end", false, TAG_LONG,
                             static_cast<int64_t>(m.end() + offset), 0, 0);
  auto* groups = culebra_runtime_array_new();  // [0]=whole match, then 1..n
  for (size_t i = 0; i <= m.group_count(); i++) {
    auto gv = _jit_regex_group(m.group(i), offset);
    culebra_runtime_array_push(groups, gv.tag, gv.data);
  }
  culebra_runtime_object_set(o, "groups", false, TAG_ARRAY,
                             reinterpret_cast<int64_t>(groups), 0, 0);
  auto* named_o = culebra_runtime_object_new();
  for (const auto& kv : named) {
    auto gv = _jit_regex_group(m.group(kv.second), offset);
    culebra_runtime_object_set(named_o, kv.first.c_str(), false, gv.tag, gv.data,
                               0, 0);
  }
  culebra_runtime_object_set(o, "named", false, TAG_OBJECT,
                             reinterpret_cast<int64_t>(named_o), 0, 0);
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
  return m.matched() ? _jit_regex_match(m, re->named_groups())
                     : _ns_adapt::v_nil();
}
inline JitValue _ns_regex_match(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  auto m = re->match(_ns_adapt::require_sv(a[1], "s", "StringLike"));
  return m.matched() ? _jit_regex_match(m, re->named_groups())
                     : _ns_adapt::v_nil();
}
inline JitValue _ns_regex_find_all(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  auto* arr = culebra_runtime_array_new();
  for (auto m : re->find_all(_ns_adapt::require_sv(a[1], "s", "StringLike"))) {
    auto mv = _jit_regex_match(m, re->named_groups());
    culebra_runtime_array_push(arr, mv.tag, mv.data);
  }
  return _ns_adapt::v_array(arr);
}
// find_all_str(pattern, s) -> [String]: matched texts only, no Match objects
// (the per-match Object structure dominated match-dense workloads). See interp.
inline JitValue _ns_regex_find_all_str(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  auto* arr = culebra_runtime_array_new();
  for (auto m : re->find_all(_ns_adapt::require_sv(a[1], "s", "StringLike"))) {
    culebra_runtime_array_push(
        arr, TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(m.str())));
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
  for (auto m : re->find_all(_ns_adapt::require_sv(a[1], "s", "StringLike"))) {
    culebra_runtime_array_push(arr, TAG_LONG, static_cast<int64_t>(m.begin()));
    culebra_runtime_array_push(arr, TAG_LONG, static_cast<int64_t>(m.end()));
  }
  return _ns_adapt::v_array(arr);
}
inline JitValue _ns_regex_replace_all(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  std::string out = re->replace_all(_ns_adapt::require_sv(a[1], "s", "StringLike"),
                                    _ns_adapt::require_sv(a[2], "repl", "StringLike"));
  return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(out))};
}
// replace_first(pattern, s, repl) -> String: like replace_all but only the
// leftmost match. Same $-template grammar (regexlib::replace_first).
inline JitValue _ns_regex_replace_first(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  std::string out = re->replace_first(_ns_adapt::require_sv(a[1], "s", "StringLike"),
                                      _ns_adapt::require_sv(a[2], "repl", "StringLike"));
  return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(out))};
}
// find_from(pattern, s, pos) -> { m: Match|nil, next: Int }. See the interp
// twin in stdlib_interp.h: one stateless find_at scan step (absolute offsets,
// engine-owned empty-match resume rule, anchors see the full subject).
inline JitValue _ns_regex_find_from(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  std::string s(_ns_adapt::require_sv(a[1], "s", "StringLike"));
  int64_t pos = a[2].data;
  auto* out = culebra_runtime_object_new();
  auto r = re->find_at(s, pos < 0 ? s.size() + 1 : static_cast<size_t>(pos));
  if (r.m.matched()) {
    auto mv = _jit_regex_match(r.m, re->named_groups());
    culebra_runtime_object_set(out, "m", false, mv.tag, mv.data, 0, 0);
  } else {
    culebra_runtime_object_set(out, "m", false, TAG_NIL, 0, 0, 0);
  }
  culebra_runtime_object_set(out, "nxt", false, TAG_LONG,
                             static_cast<int64_t>(r.next_pos), 0, 0);
  return _ns_adapt::v_object(out);
}
inline JitValue _ns_regex_split(JitValue* a, int64_t) {
  auto re = _jit_regex_compile(_ns_adapt::require_sv(a[0], "pattern", "StringLike"));
  std::string s(_ns_adapt::require_sv(a[1], "s", "StringLike"));
  auto* arr = culebra_runtime_array_new();
  size_t cursor = 0;
  for (auto m : re->find_all(s)) {
    auto piece = s.substr(cursor, m.begin() - cursor);
    culebra_runtime_array_push(
        arr, TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(piece)));
    cursor = m.end();
  }
  auto last = s.substr(cursor);
  culebra_runtime_array_push(
      arr, TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(last)));
  return _ns_adapt::v_array(arr);
}

// NsParamMeta — the per-parameter view for stdlib methods that accept
// kwargs / have defaults — is exactly the canonical signature row
// (canon_sigs.h, generated from the interp's own parameter lists while both
// engines existed; alias declared above JitExtension), never hand-authored
// per method, so the JIT binder cannot drift from the canonical spec. The
// resolver checks `n_pos` against [min_arity, max_arity] up front — before
// merging splats/kwargs — as the strict_arity rule requires, and an
// `args_rest_idx >= 0` method collects every positional into an Array at
// that slot (range/iota). A default is converted fresh to a JitValue per
// use (so heap defaults like "" stay per-call owned); see
// _jit_default_from_canon.

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
// The JIT binder's per-parameter view (NsParamMeta) is read, once per
// process, from the canonical signature table (canon_sigs.h) — generated
// from the interp's own FunctionValue::Parameter lists, never hand-authored
// — so the binders cannot drift. Only the choice of *which* methods use a
// kwarg/default slab (a JIT codegen-strategy decision, see
// _ns_method_uses_kwarg_slab) is JIT-side; the param data all flows from
// the canonical spec.

// A fresh JitValue for a canonical stdlib default. Stdlib defaults are
// scalars (nil / bool / long / float / string); heap strings are re-allocated
// per call so each binding owns its own +1.
inline JitValue _jit_default_from_canon(const CanonParam& p) {
  switch (p.default_kind) {
    case CanonDefault::Nil: return {TAG_NIL, 0};
    case CanonDefault::Bool: return {TAG_BOOL, p.default_bits};
    case CanonDefault::Long: return {TAG_LONG, p.default_bits};
    case CanonDefault::Float:
      return jit_float(std::bit_cast<double>(p.default_bits));
    case CanonDefault::Str:
      return {TAG_STRING,
              reinterpret_cast<int64_t>(_culebra_heap_str(p.default_str))};
    default:
      throw culebra::CulebraError("InternalError",
          "unsupported stdlib default value", 0, 0);
  }
}

// Resolve an NsMethod to its canonical signature, or null. One registry,
// built once (lock-free reads after init): the generated rows
// (kCanonNsSigs) seeded first, then the wrap.h-declared rows — which
// register at static-init time so they cannot be in a generated file —
// synthesized from the wrap registry (always all-required positionals, see
// WrappedNsRow). Seeding order is the collision policy: a wrap row can
// never shadow a generated one.
inline const CanonSig* _canon_sig(const NsMethod* m) {
  struct Registry {
    std::deque<std::vector<CanonParam>> wrap_params;  // stable addresses
    std::deque<CanonSig> wrap_sigs;
    std::unordered_map<std::string, const CanonSig*> table;
  };
  static const Registry reg = [] {
    Registry r;
    r.table.reserve(std::size(culebra::kCanonNsSigs) +
                    culebra::wrapped_ns_rows().size());
    for (const auto& s : culebra::kCanonNsSigs)
      r.table.emplace(culebra::canon_sig_key(s.ns, s.sub, s.name), &s);
    for (const auto& row : culebra::wrapped_ns_rows()) {
      auto& ps = r.wrap_params.emplace_back();
      ps.reserve(row.param_names.size());
      for (size_t i = 0; i < row.param_names.size(); i++) {
        ps.push_back(CanonParam{row.param_names[i], false, false, false,
                                false, false, row.param_types[i],
                                CanonDefault::None, 0, {}});
      }
      int n = static_cast<int>(ps.size());
      r.wrap_sigs.push_back(CanonSig{row.ns, row.sub, row.name,
                                     ps.empty() ? nullptr : ps.data(), n,
                                     row.return_type, n, n, false, -1, -1,
                                     -1});
      r.table.emplace(culebra::canon_sig_key(row.ns, row.sub, row.name),
                      &r.wrap_sigs.back());
    }
    return r;
  }();
  auto it = reg.table.find(culebra::canon_sig_key(
      m->ns ? m->ns : "", m->sub ? m->sub : "", m->name));
  return it == reg.table.end() ? nullptr : it->second;
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
  if (ns == "FS")       return nm == "remove" || nm == "copy" ||
                               nm == "chown" || nm == "watch";
  if (ns == "File")     return nm == "open" || nm == "with";
  if (ns == "Parallel") return nm == "map" || nm == "each" ||
                                nm == "map_settled" || nm == "race";
  if (ns == "Http")     return true;  // all Http methods take kwargs
  if (ns == "Net")      return true;  // host/timeout/backlog all bind by name
  if (ns == "JSON")     return nm == "stringify" || nm == "parse";
  if (ns == "CSV")      return nm == "parse" || nm == "stringify";
  if (ns == "TOML")     return nm == "stringify";  // sort_keys default
  if (ns == "Env")      return nm == "load";  // path/override defaults
  if (ns == "Sys")      return nm == "env";  // fallback default
  if (ns == "Compress") return nm == "deflate";  // level default
  if (ns == "IO")       return nm == "println";  // arg defaults to ""
  if (ns.empty())       return nm == "range" || nm == "iota" ||
                               nm == "grid" || nm == "println" ||
                               nm == "to_long";  // bare globals
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
inline const JitParamMeta* _jit_ns_introspect_meta(JitClosure* cls);

// Does this positional count fit the method's shape? An args-rest method
// (range/iota) absorbs any number, one with param metadata takes required
// through full, and one without keeps its fixed arity. The single source
// for the dispatch's own ArityError and for the closure trampoline's gate
// — the interp's binder counts before it binds, so a wrong count outranks
// a per-parameter type error.
inline bool _ns_positional_count_ok(const NsMethod* m, int64_t n) {
  if (const NsParamMeta* pm = _ns_meta(m)) {
    return pm->args_rest_idx >= 0 ||
           (n >= pm->min_arity && n <= pm->max_arity);
  }
  return m->arity < 0 || n == m->arity;
}

// NsMethod rows for wrap.h-declared classes (wrapped_ns_rows), built
// lazily once — after static-init froze the registry, so the c_str
// pointers into its strings are stable — and merged into every table
// consumer below alongside the static kNsMethods rows. The class name
// rides in `sub` (a nested `Ns.Class.method`, slow-path only, the
// Encoding.html shape), and the param spec is synthesized from the same
// registry rows by _canon_sig (a wrap method is always all-required
// positionals — see WrappedNsRow), so the calling-convention single
// source covers generated bindings unchanged.
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

// `Embed.dir(name)` — handle {__embed_dir__: true, name} + read/exists, also
// consumed by `srv.static`. Mirrors interp make_embed_dir_handle; the
// bake-vs-disk decision happens in vfs.h's open_embed_dir (table present →
// embedded, else live disk), so the handle just carries the name.
CULEBRA_RT_INLINE std::string _jit_embed_dir_name(JitObject* h) {
  size_t i = h->find_slot("name");
  if (i == static_cast<size_t>(-1)) return {};
  JitValue v = h->slots[i].value;
  return std::string(_culebra_str_view(v.tag, v.data));
}

CULEBRA_RT_INLINE void _jit_embed_dir_read(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                               int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "path");
  if (args[0].tag != TAG_STRING)
    _jit_file_param_type_error(self, "path", "String", 0);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  std::string name = _jit_embed_dir_name(reinterpret_cast<JitObject*>(self.data));
  std::string path(_culebra_str_view(args[0].tag, args[0].data));
  std::string bytes;
  if (!culebra::embed_dir_read(name, path, bytes))
    throw culebra::CulebraError(
        "IOError", std::format("Embed.dir.read: '{}' has no '{}'", name, path),
        _jit_call_site_line, _jit_call_site_col);
  { *__ret = {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(bytes))}; return; }
}

CULEBRA_RT_INLINE void _jit_embed_dir_exists(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                                 int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  if (!_jit_file_arg_present(n, args, 0)) _jit_file_missing_arg(self, "path");
  if (args[0].tag != TAG_STRING)
    _jit_file_param_type_error(self, "path", "String", 0);
  _JitValueGuard self_guard{static_cast<int8_t>(self.tag), self.data};
  bool ok = culebra::embed_dir_exists(
      _jit_embed_dir_name(reinterpret_cast<JitObject*>(self.data)),
      _culebra_str_view(args[0].tag, args[0].data));
  { *__ret = {TAG_BOOL, ok ? 1 : 0}; return; }
}

inline JitValue _jit_make_embed_dir_handle(const std::string& name) {
  auto* o = culebra_runtime_object_new();
  culebra_runtime_object_set(o, "__embed_dir__", false, TAG_BOOL, 1, 0, 0);
  culebra_runtime_object_set(
      o, "name", false, TAG_STRING,
      reinterpret_cast<int64_t>(_culebra_heap_str(name)), 0, 0);
  static const JitParamMeta* path_meta =
      _jit_make_handle_meta({"path"}, {false});
  _jit_handle_bind_method(o, "read", _jit_embed_dir_read, 1, path_meta);
  _jit_handle_bind_method(o, "exists", _jit_embed_dir_exists, 1, path_meta);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(o)};
}

inline JitValue _ns_embed_dir(JitValue* args, int64_t n) {
  (void)n;
  return _jit_make_embed_dir_handle(
      std::string(_culebra_str_view(args[0].tag, args[0].data)));
}

inline const NsMethod kNsMethods[] = {
  {"Embed",  "dir",       1, &_ns_embed_dir, nullptr, "String", "name"},

  {"IO",     "inspect",   1, &_ns_io_inspect},
  {"IO",     "print",     1, &_ns_io_print},
  {"IO",     "println",   1, &_ns_io_println},
  {"IO",     "input",     0, &_ns_io_input},
  {"IO",     "stdin",     0, &_ns_io_stdin},
  {"IO",     "einspect",  1, &_ns_io_einspect},
  {"IO",     "eprint",    1, &_ns_io_eprint},
  {"IO",     "eprintln",  1, &_ns_io_eprintln},
  {"IO",     "stdin_is_terminal",  0, &_ns_io_stdin_is_terminal},
  {"IO",     "stdout_is_terminal", 0, &_ns_io_stdout_is_terminal},
  {"IO",     "stderr_is_terminal", 0, &_ns_io_stderr_is_terminal},

  {"Math",   "abs",       1, &_ns_math_abs},
  {"Math",   "min",      -1, &_ns_math_min},
  {"Math",   "max",      -1, &_ns_math_max},
  {"Math",   "pow",       2, &_ns_math_pow},
  {"Math",   "sign",      1, &_ns_math_sign},
  {"Math",   "clamp",     3, &_ns_math_clamp},
  {"Math",   "wrap",      2, &_ns_math_wrap},
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

  {"FS",     "join",     -1, &_ns_fs_join},
  {"FS",     "basename",  1, &_ns_fs_basename, nullptr, "String|Path", "path"},
  {"FS",     "dirname",   1, &_ns_fs_dirname, nullptr, "String|Path", "path"},
  {"FS",     "extension", 1, &_ns_fs_extension, nullptr, "String|Path", "path"},
  {"FS",     "stem",      1, &_ns_fs_stem, nullptr, "String|Path", "path"},
  {"FS",     "exists",    1, &_ns_fs_exists, nullptr, "String|Path", "path"},
  {"FS",     "is_file",   1, &_ns_fs_is_file, nullptr, "String|Path", "path"},
  {"FS",     "is_dir",    1, &_ns_fs_is_dir, nullptr, "String|Path", "path"},
  {"FS",     "read",      1, &_ns_fs_read, nullptr, "String|Path", "path"},
  {"FS",     "write",     2, &_ns_fs_write},
  {"FS",     "size",      1, &_ns_fs_size, nullptr, "String|Path", "path"},
  {"FS",     "list_dir",  1, &_ns_fs_list_dir, nullptr, "String|Path", "path"},
  {"FS",     "mkdir",     1, &_ns_fs_mkdir},
  {"FS",     "remove",    1, &_ns_fs_remove},
  {"FS",     "stat",      1, &_ns_fs_stat},
  {"FS",     "chmod",     2, &_ns_fs_chmod},
  {"FS",     "chown",     1, &_ns_fs_chown},
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
  {"FS",     "watch",     1, &_ns_fs_watch},

  {"File",   "open",      1, &_ns_file_open},
  {"File",   "with",      2, &_ns_file_with},

  {"Random", "seed",            1, &_ns_random_seed},
  {"Random", "int",             2, &_ns_random_int},
  {"Random", "uniform",         2, &_ns_random_uniform},
  {"Random", "gauss",           2, &_ns_random_gauss},
  {"Random", "shuffle",         1, &_ns_random_shuffle, nullptr, "Array", "a"},
  {"Random", "weighted_choice", 2, &_ns_random_weighted_choice},
  {"Random", "choice",          1, &_ns_random_choice},

  {"Sys",    "exit",    1, &_ns_sys_exit},
  {"Sys",    "env",     1, &_ns_sys_env, nullptr, "String", "name"},
  {"Sys",    "getcwd",  0, &_ns_sys_getcwd},
  {"Sys",    "chdir",   1, &_ns_sys_chdir, nullptr, "String", "path"},
  {"Sys",    "set_env", 2, &_ns_sys_set_env, nullptr, "String", "name"},
  {"Sys",    "time",    0, &_ns_sys_time},

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
  {"_Regex", "replace_first",3, &_ns_regex_replace_first},
  {"_Regex", "split",       2, &_ns_regex_split},

  {"Net",    "connect", 2, &_ns_net_connect},
  {"Net",    "listen",  1, &_ns_net_listen},
  {"Net",    "udp",     0, &_ns_net_udp},
  {"Net",    "resolve", 1, &_ns_net_resolve},
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
  {"Http",   "client",  1, &_ns_http_client},
  {"Http",   "server",  0, &_ns_http_server},
  {"Http",   "ws",      1, &_ns_http_ws},
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
  {"Compress", "deflate",  1, &_ns_compress_deflate, nullptr, "String", "data"},

  {"Hash", "sha256",      1, &_ns_hash_sha256, nullptr, "String", "data"},
  {"Hash", "sha1",        1, &_ns_hash_sha1,   nullptr, "String", "data"},
  {"Hash", "sha512",      1, &_ns_hash_sha512, nullptr, "String", "data"},
  {"Hash", "md5",         1, &_ns_hash_md5,    nullptr, "String", "data"},
  {"Hash", "hmac_sha256", 2, &_ns_hash_hmac_sha256, nullptr, "String", "key"},
  {"Hash", "hmac_sha1",   2, &_ns_hash_hmac_sha1,   nullptr, "String", "key"},
  {"Hash", "hmac_sha512", 2, &_ns_hash_hmac_sha512, nullptr, "String", "key"},

  {"CSV", "parse",     1, &_ns_csv_parse,     nullptr, "String", "text"},
  {"CSV", "stringify", 1, &_ns_csv_stringify, nullptr, "Array",  "rows"},
#if defined(CULEBRA_SQLITE_ENABLED)
  {"SQLite", "open",    1, &_ns_sqlite_open,    nullptr, "String", "path"},
  {"SQLite", "version", 0, &_ns_sqlite_version},
#endif
  {"TOML", "parse",     1, &_ns_toml_parse,     nullptr, "String", "text"},
  {"TOML", "stringify", 1, &_ns_toml_stringify, nullptr, "Object", "v"},

  {"Env", "parse", 1, &_ns_env_parse, nullptr, "String", "text"},
  {"Env", "load",  0, &_ns_env_load,  nullptr, "String", "path"},

  {"UUID", "v4", 0, &_ns_uuid_v4},
  {"UUID", "v7", 0, &_ns_uuid_v7},

  {"String", "from_code_point", 1, &_ns_string_from_code_point, nullptr, "Long", "cp"},
  {"String", "from_bytes", 1, &_ns_string_from_bytes, nullptr, "Array", "bytes"},
  {"String", "from_code_points", 1, &_ns_string_from_code_points, nullptr, "Array", "cps"},

  {"Tensor", "zeros",    -1, &_ns_tensor_zeros},
  {"Tensor", "ones",     -1, &_ns_tensor_ones},
  {"Tensor", "randn",    -1, &_ns_tensor_randn},
  {"Tensor", "eval",     -1, &_ns_tensor_eval},
  {"Tensor", "from_csv",  1, &_ns_tensor_from_csv, nullptr, "String", "path"},
  {"Tensor", "from",      1, &_ns_tensor_from, nullptr, "Array",  "a"},
  {"Tensor", "concat",    1, &_ns_tensor_concat, nullptr, "Array", "parts"},
  {"Tensor", "no_grad",   1, &_ns_tensor_no_grad, nullptr, "Function", "fn"},
  {"Tensor", "use_cpu",       0, &_ns_tensor_use_cpu},
  {"Tensor", "use_gpu",       0, &_ns_tensor_use_gpu},
  {"Tensor", "use_auto",      0, &_ns_tensor_use_auto},
  {"Tensor", "gpu_available", 0, &_ns_tensor_gpu_available},
  {"Tensor", "device",        0, &_ns_tensor_device},

  {"_Time",  "now_nanos",        0, &_ns_time_now_nanos},
  {"_Time",  "monotonic",        0, &_ns_time_monotonic},
  {"_Time",  "sleep",            1, &_ns_time_sleep},
  {"_Time",  "from_iso_nanos",   1, &_ns_time_from_iso_nanos},
  {"_Time",  "parse_nanos",      2, &_ns_time_parse_nanos},
  {"_Time",  "iso_nanos",        2, &_ns_time_iso_nanos},
  {"_Time",  "format_nanos",     3, &_ns_time_format_nanos},
  {"_Time",  "parts_nanos",      2, &_ns_time_parts_nanos},
  {"_Time",  "from_parts_nanos", 2, &_ns_time_from_parts_nanos},
  {"_Time",  "weekday_nanos",    2, &_ns_time_weekday_nanos},
  {"_Time",  "add_nanos",        8, &_ns_time_add_nanos},
  {"_Time",  "start_of_nanos",   3, &_ns_time_start_of_nanos},

  {"_Term",  "cols",        0, &_ns_term_cols},
  {"_Term",  "rows",        0, &_ns_term_rows},
  {"_Term",  "raw_on",      0, &_ns_term_raw_on},
  {"_Term",  "raw_off",     0, &_ns_term_raw_off},
  {"_Term",  "flush",       0, &_ns_term_flush},
  {"_Term",  "color_level", 0, &_ns_term_color_level},
  {"_Term",  "width",       1, &_ns_term_width},
  {"_Term",  "resized",     0, &_ns_term_resized},
  {"_Term",  "read_key",    1, &_ns_term_read_key},
  {"_Term",  "attach_tty",  0, &_ns_term_attach_tty},

  {"_Canvas", "init",            2,  &_ns_canvas_init},
  {"_Canvas", "ttf_load",        1,  &_ns_canvas_ttf_load},
  {"_Canvas", "ttf_free",        1,  &_ns_canvas_ttf_free},
  {"_Canvas", "ttf_glyph",       6,  &_ns_canvas_ttf_glyph},
  {"_Canvas", "ttf_glyph_screen", 6, &_ns_canvas_ttf_glyph_screen},
  {"_Canvas", "ttf_advance",     3,  &_ns_canvas_ttf_advance},
  {"_Canvas", "ttf_ascent",      2,  &_ns_canvas_ttf_ascent},
  {"_Canvas", "screen_width",    0,  &_ns_canvas_screen_width},
  {"_Canvas", "screen_height",   0,  &_ns_canvas_screen_height},
  {"_Canvas", "get_screen_pixel", 2, &_ns_canvas_get_screen_pixel},
  {"_Canvas", "screen_scale",    0,  &_ns_canvas_screen_scale},
  {"_Canvas", "clear",           1,  &_ns_canvas_clear},
  {"_Canvas", "set_pixel",       3,  &_ns_canvas_set_pixel},
  {"_Canvas", "get_pixel",       2,  &_ns_canvas_get_pixel},
  {"_Canvas", "rect",            6,  &_ns_canvas_rect},
  {"_Canvas", "line",            5,  &_ns_canvas_line},
  {"_Canvas", "ellipse",         6,  &_ns_canvas_ellipse},
  {"_Canvas", "triangle",        8,  &_ns_canvas_triangle},
  {"_Canvas", "polygon",         3,  &_ns_canvas_polygon},
  {"_Canvas", "font_load",       1,  &_ns_canvas_font_load},
  {"_Canvas", "glyph",           6,  &_ns_canvas_glyph},
  {"_Canvas", "sprite_load",     3,  &_ns_canvas_sprite_load},
  {"_Canvas", "sprite_from_png", 1,  &_ns_canvas_sprite_from_png},
  {"_Canvas", "sprite_to_png",   1,  &_ns_canvas_sprite_to_png},
  {"_Canvas", "sprite_width",    1,  &_ns_canvas_sprite_width},
  {"_Canvas", "sprite_height",   1,  &_ns_canvas_sprite_height},
  {"_Canvas", "sprite_blank",    3,  &_ns_canvas_sprite_blank},
  {"_Canvas", "sprite_free",     1,  &_ns_canvas_sprite_free},
  {"_Canvas", "target",          1,  &_ns_canvas_target},
  {"_Canvas", "blit",            8,  &_ns_canvas_blit},
  {"_Canvas", "blit_scaled",     11, &_ns_canvas_blit_scaled},
  {"_Canvas", "present",         0,  &_ns_canvas_present},
  {"_Canvas", "buttons",         0,  &_ns_canvas_buttons},
  {"_Canvas", "mouse_x",         0,  &_ns_canvas_mouse_x},
  {"_Canvas", "mouse_y",         0,  &_ns_canvas_mouse_y},
  {"_Canvas", "mouse_buttons",   0,  &_ns_canvas_mouse_buttons},
  {"_Canvas", "key",             1,  &_ns_canvas_key},
  {"_Canvas", "key_pop",         0,  &_ns_canvas_key_pop},
  {"_Canvas", "char_pop",        0,  &_ns_canvas_char_pop},
  {"_Canvas", "closing",         0,  &_ns_canvas_closing},
  {"_Canvas", "windowed",        0,  &_ns_canvas_windowed},
  {"_Canvas", "title",           1,  &_ns_canvas_title},
  {"_Canvas", "tone",            10, &_ns_canvas_tone},
  {"_Canvas", "music_play",      4,  &_ns_canvas_music_play},
  {"_Canvas", "music_stop",      0,  &_ns_canvas_music_stop},
  {"_Canvas", "music_pause",     0,  &_ns_canvas_music_pause},
  {"_Canvas", "music_resume",    0,  &_ns_canvas_music_resume},
  {"_Canvas", "music_volume",    1,  &_ns_canvas_music_volume},
  {"_Canvas", "music_seek",      1,  &_ns_canvas_music_seek},
  {"_Canvas", "music_playing",   0,  &_ns_canvas_music_playing},
  {"_Canvas", "sound_load",      1,  &_ns_canvas_sound_load},
  {"_Canvas", "sound_play",      2,  &_ns_canvas_sound_play},
  {"_Canvas", "sound_stop",      1,  &_ns_canvas_sound_stop},
  {"_Canvas", "sound_playing",   1,  &_ns_canvas_sound_playing},
  {"_Canvas", "sound_free",      1,  &_ns_canvas_sound_free},
  {"_Canvas", "width",           0,  &_ns_canvas_width},
  {"_Canvas", "height",          0,  &_ns_canvas_height},
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

inline JitValue _ns_const_pi()  { return _ns_adapt::v_float(std::numbers::pi); }
inline JitValue _ns_const_e()   { return _ns_adapt::v_float(std::numbers::e); }
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
      slab[i] = _jit_default_from_canon(pm->params[i]);
    } else {
      cleanup();
      throw culebra::CulebraError("ArityError",
          culebra::missing_required_arg_message(pm->params[i].name),
          line, col);
    }
  }
  if (!merged.empty()) {
    auto bad = std::string(culebra::canonical_unknown_kwarg(merged));
    cleanup();
    throw culebra::CulebraError("TypeError",
        culebra::unknown_kwarg_message(bad), line, col);
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
// `bline/bcol` is the call's boundary position (_jit_call_boundary_*,
// snapshotted by the trampoline at entry so a nested call inside the body
// can't clobber it): where the interp's eval() would stamp a positionless
// error escaping this call. It equals the call site everywhere except a
// UFCS site, whose explicit errors (arity, param types) report at the
// argument list while its boundary is the postfix chain's head.
inline JitValue _jit_ns_method_dispatch(const NsMethod* m, int64_t n_args,
                                        JitValue* args, int64_t line,
                                        int64_t col, int64_t arg0_line,
                                        int64_t arg0_col, int64_t bline,
                                        int64_t bcol,
                                        const NsParamMeta* pm_hint = nullptr) {
  // The trampoline (the only caller with `pm` already in hand, needed for its
  // own arity gate) passes it through to skip a second _ns_meta lookup keyed
  // on the same `m`; the other callers leave this null and get it here.
  const NsParamMeta* pm = pm_hint ? pm_hint : _ns_meta(m);
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
      if (!_ns_positional_count_ok(m, n_args)) {
        release_args();
        // Too few → required count; too many → the cap (interp parity).
        _ns_adapt::arity_error(
            m->ns, m->name,
            n_args > pm->max_arity ? pm->max_arity : pm->min_arity, n_args,
            line, col);
      }
    } else if (!_ns_positional_count_ok(m, n_args)) {
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
        !_culebra_value_matches_type(args[0].tag, args[0].data,
                                     m->arg0_type)) {
      const char* got = culebra_runtime_type_of(args[0].tag);
      release_args();
      culebra::throw_runtime_error_at(
          "TypeError", culebra::type_mismatch_message(m->arg0_type, got),
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
    _jit_backfill_error_pos(e, bline, bcol);
    throw;
  }
}

inline void _jit_ns_method_trampoline(
    JitValue* __ret, JitClosure* cls, int8_t /*self_tag*/,
    int64_t /*self_data*/, int64_t n_args, JitValue* args) {
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
  // A wrong positional count is the interp's first answer (its binder
  // counts before it binds), so leave the report to the dispatch's arity
  // check below rather than type-checking arguments the call cannot fill.
  // `pm` is resolved once here and handed to the dispatch below, which would
  // otherwise look it up again under the same key.
  const NsParamMeta* pm = _ns_meta(m);
  if (_jit_argpos_n > 0 && _ns_positional_count_ok(m, n_args)) {
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
  { *__ret = _jit_ns_method_dispatch(m, n_args, args, _jit_call_site_line,
                                 _jit_call_site_col, _jit_call_arg0_line,
                                 _jit_call_arg0_col, _jit_call_boundary_line,
                                 _jit_call_boundary_col, pm); return; }
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
  // The merge retained each splat value it kept (kwargs transfer their +1 into
  // `merged`); the splat *Objects* themselves — +1-owned by the caller's slab
  // and handed to us — are done being read, so drop them here. Covers both the
  // args-rest (range/iota) and fixed-param success paths below. The pre-merge
  // error paths already release them via `release_all`, so this runs only once.
  for (int64_t i = 0; i < n_splat; i++)
    _culebra_value_release_impl(splat_objs[i].tag, splat_objs[i].data);

  // Args-rest method (range/iota): positionals collect into the rest Array
  // instead of binding fixed slots, so the variadic 1-2 start/end args coexist
  // with the keyword-only `step`. The shared builder yields the same canonical
  // slab the bare-positional trampoline does; dispatch the adapter directly
  // (the slab is already canonical) and release it after.
  if (pm->args_rest_idx >= 0) {
    auto slab = _jit_ns_build_args_rest_slab(pm, n_pos, positional, merged,
                                             line, col);
    *out = _jit_at_pos(line, col,
                       [&] { return _jit_ns_dispatch_owned_slab(m, slab); });
    return true;
  }

  int n = pm->n_params;
  std::vector<JitValue> slab(n);
  // The slab lives in a std::vector's heap buffer, which the conservative
  // collector does not scan. Refcounted values in it are held by their +1,
  // but a traced-only String/StringView has no refcount — a default-valued
  // string param, or an adapter-internal string temporary staged here, is
  // then unreachable from any root and a GC_STRESS collect would sweep it
  // before the adapter reads it (crash). Pause collection across slab build
  // and dispatch, mirroring _jit_ns_dispatch_owned_slab's args-rest path.
  culebra::gc::Heap::CollectPause _pause(_gc_heap());
  std::vector<bool> filled(n, false);
  // Everything this frame still owns when the positional pass gives up at
  // `i`: the slab cells already taken, the positionals not yet moved into
  // one, and every merged keyword.
  auto release_partial_bind = [&](int64_t i) {
    for (int64_t k = 0; k < i; k++)
      _culebra_value_release_impl(slab[k].tag, slab[k].data);
    for (int64_t k = i; k < n_pos; k++)
      _culebra_value_release_impl(positional[k].tag, positional[k].data);
    for (auto& [_, v] : merged)
      _culebra_value_release_impl(v.tag, v.data);
  };
  // Positional args bind leftmost params; reject if also given by keyword.
  for (int64_t i = 0; i < n_pos && i < n; i++) {
    if (merged.count(pm->params[i].name)) {
      release_partial_bind(i);
      throw culebra::CulebraError("TypeError",
          culebra::positional_kw_conflict_message(pm->params[i].name),
          line, col);
    }
    // The positional's declared type. The JIT's direct kwargs path emits a
    // per-argument check of its own before the call (compile_ns_method_kwargs)
    // — nothing else does, so a call through a value (`let f = Sys.env; f(42,
    // fallback: "x")`) or from the VM, which has no such path, bound an
    // ill-typed positional where the interp's binder throws. Reported at the
    // argument's own position when the caller published one, as the
    // as-value trampoline does, else at the call.
    if (!pm->params[i].type.empty() &&
        !_culebra_value_matches_type(positional[i].tag, positional[i].data,
                                     pm->params[i].type)) {
      release_partial_bind(i);
      int64_t l = line, cl = col;
      if (i < _jit_argpos_n) {
        l = _jit_argpos_line[i];
        cl = _jit_argpos_col[i];
      }
      throw culebra::CulebraError(
          "TypeError",
          std::format("type error: parameter '{}' expects {}",
                      pm->params[i].name, pm->params[i].type),
          l, cl);
    }
    slab[i] = positional[i];
    filled[i] = true;
  }
  if (n_pos > n) {  // too many positionals
    release_partial_bind(n);
    _ns_adapt::arity_error(m->ns, m->name, n, n_pos, line, col);
  }
  // Remaining params: from merged kwargs, else default, else ArityError.
  for (int i = static_cast<int>(n_pos); i < n; i++) {
    auto it = merged.find(pm->params[i].name);
    if (it != merged.end()) {
      slab[i] = it->second;
      filled[i] = true;
      merged.erase(it);
      // A keyword-supplied value's target slot isn't known until this
      // name lookup, so it never went through the compile-time
      // per-argument-position check the positional path gets (see
      // compile_ns_method_kwargs) — this is its only type check. Position
      // is the call site, matching the interp binder's kwarg diagnostic.
      if (!pm->params[i].type.empty() &&
          !_culebra_value_matches_type(slab[i].tag, slab[i].data,
                                       pm->params[i].type)) {
        for (int k = 0; k <= i; k++)
          if (filled[k]) _culebra_value_release_impl(slab[k].tag, slab[k].data);
        for (auto& [_, v] : merged)
          _culebra_value_release_impl(v.tag, v.data);
        throw culebra::CulebraError("TypeError", std::format(
            "type error: parameter '{}' expects {}", pm->params[i].name,
            pm->params[i].type), line, col);
      }
    } else if (pm->params[i].has_default) {
      slab[i] = _jit_default_from_canon(pm->params[i]);
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
    auto bad = std::string(culebra::canonical_unknown_kwarg(merged));
    for (int k = 0; k < n; k++)
      _culebra_value_release_impl(slab[k].tag, slab[k].data);
    for (auto& [_, v] : merged)
      _culebra_value_release_impl(v.tag, v.data);
    throw culebra::CulebraError("TypeError",
        culebra::unknown_kwarg_message(bad), line, col);
  }

  // Dispatch the full-arity slab, as the args-rest path above does: it is
  // canonical by construction and its positional arity was checked up front,
  // so re-entering _jit_ns_method_dispatch would only re-run that gate — and
  // wrongly, since a keyword-only param occupies a slab slot the positional
  // cap (max_arity) deliberately excludes. `_jit_at_pos` supplies the backfill
  // for a positionless adapter error (`Http.get(params: 5)`) that the dispatch
  // would have — at the boundary, which is the call site everywhere except a
  // UFCS chain, whose errors anchor at the chain's head.
  *out = _jit_at_pos(_jit_call_boundary_line, _jit_call_boundary_col,
                     [&] { return _jit_ns_dispatch_owned_slab(m, slab); });
  return true;
}

// Closure-ABI wrapper: extract the NsMethod from the closure and resolve.
// Returns false (the hook's "not mine" signal) for non-ns closures.
inline bool _jit_ns_kwarg_resolve(
    JitClosure* cls, JitValue /*self_val*/, int64_t n_pos, JitValue* positional,
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

// Callback-arity bounds for an ns-method closure handed to a HOF. Reads the
// SAME canonical bounds interp's check_callback_arity derived (the sig's
// builtin_arity_bounds result, precomputed at generation), so both backends
// gate an ns-method callback identically. Returns false for non-ns closures.
// Goes through _ns_type_meta (pointer-keyed, covers every method) rather
// than _canon_sig — this runs once per HOF call, and the string-keyed
// registry lookup would allocate on that path.
inline bool _jit_ns_callback_arity(JitClosure* cls, int64_t* cb_min,
                                   int64_t* cb_max) {
  if (cls->fn_ptr != reinterpret_cast<void*>(_jit_ns_method_trampoline)) {
    return false;
  }
  const auto* m = reinterpret_cast<const NsMethod*>(
      cls->captures[0]->value.data);
  const NsParamMeta* s = _ns_type_meta(m);
  if (!s) return false;  // no canonical sig → fall back to closure arity
  *cb_min = s->min_arity;
  *cb_max = s->variadic ? -1 : s->max_arity;
  return true;
}

// Install the kwarg + callback-arity hooks once, before any JIT call runs.
inline const bool _jit_ns_kwarg_hook_installed = [] {
  _jit_ns_kwarg_hook = &_jit_ns_kwarg_resolve;
  _jit_native_meta_hook = &_jit_ns_introspect_meta;  // defined below
  _jit_ns_callback_arity_hook = &_jit_ns_callback_arity;
  return true;
}();

inline JitObject* _jit_build_namespace_object(std::string_view ns_name) {
  // Build with collection paused: the method closures and sub-namespace
  // objects are registered but not reachable from any root until they are
  // slotted, so a GC_STRESS collect mid-build would sweep them.
  culebra::gc::Heap::CollectPause _pause(_gc_heap());
  auto* obj = culebra_runtime_object_new();
  // Sys leads with its dynamic members — the interpreter's make_sys_namespace
  // registers them before the table-driven ones, and `keys()` is insertion
  // order on both backends.
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
    // `Sys.script` — the entry script's absolute path, or nil when there is no
    // source file at runtime (an AOT binary never sets the holder). Mirrors the
    // interpreter's make_sys_namespace.
    const auto& sp = culebra::main_script_path();
    obj->append_slot(
        "script",
        sp.empty()
            ? JitValue{TAG_NIL, 0}
            : JitValue{TAG_STRING,
                       reinterpret_cast<int64_t>(_culebra_heap_str(sp))},
        /*mut=*/false);
  }
  // Sub-namespace objects, created lazily and keyed by `sub` (e.g. "html").
  // Reachable from `obj` (a pinned root), so the marker keeps them + their
  // method closures alive for the program's lifetime. Kept in first-appearance
  // order: `keys()` reports insertion order, so a hash map's bucket order would
  // make the sub-namespace names come out differently from the interpreter
  // (and differently across runs).
  std::vector<std::pair<std::string_view, JitObject*>> subs;
  auto sub_object = [&](std::string_view name) {
    for (auto& [n, o] : subs)
      if (n == name) return o;
    return subs.emplace_back(name, culebra_runtime_object_new()).second;
  };
  auto add_method = [&](const NsMethod& m) {
    if (ns_name != m.ns) return;
    auto* fn = _jit_make_ns_method_closure(&m);
    JitValue fv{TAG_FUNC, reinterpret_cast<int64_t>(fn)};
    if (m.sub) {
      sub_object(m.sub)->append_slot(m.name, fv, /*mut=*/false);
    } else {
      obj->append_slot(m.name, fv, /*mut=*/false);
    }
  };
  for (auto& m : kNsMethods) add_method(m);
  for (auto& m : _wrapped_ns_methods()) add_method(m);
  // Wrapped classes with no ctor/static rows still get their (empty)
  // class sub-object, mirroring the interp's registry walk.
  for (auto& wc : culebra::wrapped_class_names()) {
    if (ns_name != wc.ns) continue;
    (void)sub_object(wc.name);
  }
  for (auto& [name, sub] : subs) {
    obj->append_slot(name, JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(sub)},
                     /*mut=*/false);
  }
  for (auto& c : kNsConstants) {
    if (ns_name != c.ns) continue;
    obj->append_slot(c.name, c.build(), /*mut=*/false);
  }
  // Tag as a builtin namespace so an unknown-member read raises AttributeError
  // (see culebra_runtime_object_get_ic), matching the interpreter.
  obj->is_namespace = true;
  obj->ns_name = _intern_str(ns_name);  // process-lifetime, NUL-terminated
  return obj;
}

struct _JitNamespaceTable {
  std::unordered_map<std::string, JitObject*> entries;
  // Bare builtin function globals (`inspect`, `type_of`, `range`, ...) used
  // as first-class values, materialized lazily as ns-method closures. Same
  // lifetime/teardown order as `entries` — both hold heap values that must
  // be released before the GC heap substate (a lower slot) is destroyed.
  std::unordered_map<std::string, JitClosure*> builtin_fns;
  // Memo for the bare function globals written in culebra (the matcher
  // family, the String helpers). Each is a member slot of its group Object in
  // `entries`, which holds the reference and is pinned for the Runtime's
  // lifetime — so this map borrows and needs no teardown of its own.
  std::unordered_map<std::string, JitClosure*> lazy_fns;
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
// only value / higher-order uses (`map(type_of)`, `let f = inspect`) reach the
// closure built here. `ns` is empty — these are not namespaced; the trampoline
// uses it only for arity-error text, which `_ns_adapt::arity_error` renders
// name-only when blank.
inline const NsMethod kBuiltinFns[] = {
  {"", "inspect",   1, &_ns_io_inspect},
  {"", "print",     1, &_ns_io_print},
  {"", "println",   1, &_ns_io_println},
  {"", "type_of",   1, &_ns_global_type_of},
  {"", "to_long",   2, &_ns_global_to_long},
  {"", "to_float",  1, &_ns_global_to_float},
  {"", "to_string", 1, &_ns_global_to_string},
  {"", "hash",      1, &_ns_global_hash},
  {"", "__eff_copy", 1, &_ns_global_eff_copy},
  {"", "__eff_abort", 1, &_ns_global_eff_abort},
  {"", "__eff_catch_abort", 1, &_ns_global_eff_catch_abort},
  {"", "range",    -1, &_ns_global_range},
  {"", "iota",     -1, &_ns_global_iota},
  {"", "repeat",    2, &_ns_global_repeat},
  {"", "grid",     -1, &_ns_global_grid},
};

namespace _ns_spec_detail {
// The NsMethod* → canonical-sig table for the rows `pred` admits (null pred
// = every row). Pointer-keyed so the per-call lookups below never build a
// string key — `_canon_sig` (string-keyed, allocates) runs only here, once
// per row at build time.
inline std::unordered_map<const NsMethod*, const NsParamMeta*> sig_table(
    bool (*pred)(const NsMethod*)) {
  std::unordered_map<const NsMethod*, const NsParamMeta*> t;
  auto add = [&](const NsMethod& nm) {
    if (pred && !pred(&nm)) return;
    if (const CanonSig* s = _canon_sig(&nm)) t.emplace(&nm, s);
    // else: canonical lookup failed — skip, callers fall back
  };
  for (const auto& nm : kNsMethods) add(nm);
  for (const auto& nm : kBuiltinFns) add(nm);
  for (const auto& nm : _wrapped_ns_methods()) add(nm);
  return t;
}
}  // namespace _ns_spec_detail

// The canonical spec for every method whose adapter consumes a kwarg/default
// slab (drives kwarg/arity dispatch), or null for the rest.
inline const NsParamMeta* _ns_meta(const NsMethod* m) {
  static const auto table = _ns_spec_detail::sig_table(
      [](const NsMethod* nm) { return _ns_method_uses_kwarg_slab(nm); });
  auto it = table.find(m);
  return it == table.end() ? nullptr : it->second;
}

// Per-positional type view for EVERY method (not gated by _ns_method_uses_
// kwarg_slab). The closure trampoline consults this to type-check each
// positional at the argument's source position — exactly like the interp binder
// — instead of letting a pure-positional adapter coerce a wrong-typed arg (e.g.
// `FS.rename(a, 5)` silently running rename on ""). Distinct from `_ns_meta`,
// which gates kwarg/arity DISPATCH; widening this type-only view to all methods
// cannot change dispatch. Same canonical source (_canon_sig) → no drift.
inline const NsParamMeta* _ns_type_meta(const NsMethod* m) {
  static const auto table = _ns_spec_detail::sig_table(nullptr);
  auto it = table.find(m);
  return it == table.end() ? nullptr : it->second;
}

// Introspection view of a native stdlib closure. `fn.params` / `.return_type`
// read a JitParamMeta keyed by fn_ptr, and every native closure shares one
// trampoline, so nothing was ever registered for them and the compiled
// engines reported an empty signature where the interpreter — reading the
// canonical FunctionValue directly — reported the real one. Derived here from
// that same canonical source, so the two cannot drift.
namespace _ns_introspect_detail {
struct NativeMeta {
  std::vector<std::string> names;      // stable storage for the cstrings
  std::vector<std::string> types;
  std::vector<const char*> name_ptrs;
  std::vector<const char*> type_ptrs;
  std::vector<uint8_t> has_default_bits;
  std::vector<uint8_t> mut_bits;
  std::string return_type;
  JitParamMeta meta{};
};

inline std::unique_ptr<NativeMeta> build(const NsMethod* m) {
  const CanonSig* sig = _canon_sig(m);
  if (!sig) return nullptr;
  auto nm = std::make_unique<NativeMeta>();
  size_t n = static_cast<size_t>(sig->n_params);
  nm->names.reserve(n);
  nm->types.reserve(n);
  nm->has_default_bits.assign((n + 7) / 8, 0);
  nm->mut_bits.assign((n + 7) / 8, 0);
  int64_t kwargs_rest_idx = -1, first_kw_only_idx = -1;
  for (size_t i = 0; i < n; i++) {
    const auto& cp = sig->params[i];
    // `*args` is a synthetic overflow slot, not a positional parameter — the
    // interp's own params view omitted it, so this one does too.
    if (cp.args_rest) continue;
    size_t k = nm->names.size();
    nm->names.emplace_back(cp.name);
    nm->types.emplace_back(cp.type);
    if (cp.has_default)
      nm->has_default_bits[k / 8] |= static_cast<uint8_t>(1u << (k % 8));
    if (cp.mut) nm->mut_bits[k / 8] |= static_cast<uint8_t>(1u << (k % 8));
    if (cp.kwargs_rest && kwargs_rest_idx < 0)
      kwargs_rest_idx = static_cast<int64_t>(k);
    if (cp.kw_only && first_kw_only_idx < 0)
      first_kw_only_idx = static_cast<int64_t>(k);
  }
  for (auto& s : nm->names) nm->name_ptrs.push_back(s.c_str());
  for (auto& s : nm->types) nm->type_ptrs.push_back(s.c_str());
  nm->return_type = std::string(sig->return_type);
  nm->meta = JitParamMeta{nm->name_ptrs.data(),
                          nm->has_default_bits.data(),
                          nm->name_ptrs.size(),
                          kwargs_rest_idx,
                          first_kw_only_idx,
                          // A native FunctionValue carries no name of its own;
                          // the interp adopts the name a member read looked
                          // up, so a namespace's method reports it and a bare
                          // global (`println`, read through no member) does
                          // not.
                          (m->ns && m->ns[0] != '\0') ? m->name : "",
                          nm->return_type.c_str(),
                          nm->mut_bits.data(),
                          nm->type_ptrs.data(),
                          // Nothing neutralizes a native's annotations, so the
                          // declared and effective views are the same one.
                          nm->type_ptrs.data(),
                          sig->min_arity,
                          sig->variadic ? -1 : sig->max_arity};
  return nm;
}
}  // namespace _ns_introspect_detail

inline const JitParamMeta* _jit_ns_introspect_meta(JitClosure* cls) {
  if (!cls || cls->fn_ptr != reinterpret_cast<void*>(_jit_ns_method_trampoline))
    return nullptr;
  const auto* m = reinterpret_cast<const NsMethod*>(
      cls->captures[0]->value.data);
  static std::vector<std::unique_ptr<_ns_introspect_detail::NativeMeta>> storage;
  static const std::unordered_map<const NsMethod*, const JitParamMeta*> table =
      [&] {
        std::unordered_map<const NsMethod*, const JitParamMeta*> t;
        auto add = [&](const NsMethod& nm) {
          auto built = _ns_introspect_detail::build(&nm);
          if (!built) return;
          t.emplace(&nm, &built->meta);
          storage.push_back(std::move(built));
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
inline JitClosure* _jit_builtin_fn_closure(_JitNamespaceTable& t,
                                           const std::string& name) {
  auto& cache = t.builtin_fns;
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
  for (auto& wc : culebra::wrapped_class_names())
    if (name == wc.ns) return true;
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

// --- Lazy source-module (Time/Regex/Term/Args/Log) builder registry ---------
//
// The interp binds these culebra-source modules lazily (initialize_lazy) and
// re-resolves them per environment, so a closure shipped to another isolate is
// rebuilt there from the same source — it never carries the native members
// that make the module Object non-Sendable. The JIT mirrors this without a
// per-runtime source eval: each module's builder is the captureless
// `fn(){ <module body>; <Module Object> }` already present in the preamble
// (e.g. `_time_module`). The JIT splice runs `_lazy_ns_register(name, builder)`
// once at entry-module top level, which records the builder's machine-code
// fn_ptr here. fn_ptrs are process-global (the same JIT-compiled body runs on
// any Runtime, like a deserialized closure's shared fn_ptr), so a child isolate
// resolves the module by rebuilding a captureless closure on its own heap and
// invoking it — exactly how run_isolate_child_jit rebuilds user closures.
inline std::mutex& _lazy_ns_builder_mutex() {
  static std::mutex m;
  return m;
}
// The builder's code, plus — on a lane whose closures share one fn_ptr — the
// chunk descriptor to run (the payload of capture 0, see
// _jit_closure_desc_hook). It is the descriptor value and not the cell
// carrying it: cells belong to the Runtime that allocated them, and every
// isolate rebuilds this builder on its own. Zero `desc` is the AST-JIT/AOT
// case, where the fn_ptr says everything.
struct _LazyNsBuilder {
  void* fn_ptr = nullptr;
  int64_t desc = 0;
};
inline std::unordered_map<std::string, _LazyNsBuilder>& _lazy_ns_builders() {
  static std::unordered_map<std::string, _LazyNsBuilder> r;
  return r;
}
inline void _lazy_ns_register_builder(const std::string& name, void* fn_ptr,
                                      int64_t desc) {
  std::lock_guard<std::mutex> lk(_lazy_ns_builder_mutex());
  _lazy_ns_builders().insert_or_assign(name, _LazyNsBuilder{fn_ptr, desc});
}
inline _LazyNsBuilder _lazy_ns_builder(const std::string& name) {
  std::lock_guard<std::mutex> lk(_lazy_ns_builder_mutex());
  auto it = _lazy_ns_builders().find(name);
  return it == _lazy_ns_builders().end() ? _LazyNsBuilder{} : it->second;
}

inline JitObject* _jit_namespace_get_or_build(const std::string& name) {
  auto& table = culebra::runtime_substate<_JitNamespaceTable>(
                    culebra::kSlotJitNamespaceTable).entries;
  auto it = table.find(name);
  if (it != table.end()) return it->second;
  // A lazy source module: build it on the current Runtime by invoking the
  // registered captureless builder closure (arity 0, no captures). The result
  // is the module Object (refcount 1); the table + pin hold that single ref for
  // the Runtime's lifetime, same discipline as the native path below.
  if (auto bd = _lazy_ns_builder(name); bd.fn_ptr) {
    // One capture where the lane needs the chunk named (the VM executor's
    // descriptor, in a cell of this Runtime's own), none where the fn_ptr is
    // the whole answer.
    auto* cls = culebra_runtime_closure_new(bd.fn_ptr, bd.desc ? 1 : 0,
                                            /*arity=*/0);
    if (bd.desc) cls->captures[0] = culebra_runtime_cell_new(TAG_LONG, bd.desc);
    JitValue r = _culebra_invoke0(cls);
    culebra_runtime_value_release(TAG_FUNC, reinterpret_cast<int64_t>(cls));
    if (r.tag != TAG_OBJECT) {
      culebra_runtime_value_release(r.tag, r.data);
      return nullptr;  // a builder must return its namespace Object
    }
    auto* obj = reinterpret_cast<JitObject*>(r.data);
    // Tag it as a closed namespace like the native path below does. Every
    // reference (entry module, closure, child isolate) resolves through here,
    // so one write per Runtime covers them all.
    if (const char* ns = culebra::lazy_namespace_static_name(name)) {
      obj->is_namespace = true;
      obj->ns_name = ns;  // static storage, outlives every Runtime
    }
    table.emplace(name, obj);
    _gc_heap().pin(obj);
    return obj;
  }
  if (!_is_known_ns(name)) return nullptr;
  auto* obj = _jit_build_namespace_object(name);
  table.emplace(name, obj);
  // Cached for the program's lifetime and reached only through this table
  // (off any scanned stack between uses), so pin it as a permanent root; the
  // marker traces its method closures as children.
  _gc_heap().pin(obj);
  return obj;
}

// Resolve a bare stdlib function global (matcher family, String helpers) to the one
// closure its group module built on this Runtime, or null if `name` is not
// one. The group Object is built + pinned by _jit_namespace_get_or_build, so
// reading the member out of its slot — rather than through property access,
// which hands back a fresh bound view each time — gives every module the same
// function value. That is the JIT's counterpart to the interp's single global
// binding, and what makes `assert_eq` captured in one module compare equal to
// `assert_eq` read in another. The result is borrowed: the group Object owns
// it and outlives every caller (see _JitNamespaceTable::lazy_fns).
inline JitClosure* _jit_lazy_fn_closure(_JitNamespaceTable& t,
                                        const std::string& name) {
  // Memo first: a name outside every group is never in it, so the group scan
  // only runs before the first resolve of a name that is.
  auto it = t.lazy_fns.find(name);
  if (it != t.lazy_fns.end()) return it->second;
  auto group = culebra::lazy_fn_group_of(name);
  if (group.empty()) return nullptr;
  auto* obj = _jit_namespace_get_or_build(std::string(group));
  if (!obj) return nullptr;
  auto slot = obj->find_slot(name);
  if (slot == static_cast<size_t>(-1)) return nullptr;
  auto member = obj->slots[slot].value;
  if (member.tag != TAG_FUNC) return nullptr;
  auto* cls = reinterpret_cast<JitClosure*>(member.data);
  t.lazy_fns.emplace(name, cls);
  return cls;
}

// The interp-vs-generated-table 1:1 check moved to interp_sig_check.h
// (Phase 4 B7-b): it reads the tree-walker's own tables, which this header
// no longer sees. TUs that hold both stacks (main.cc, culebra_rt.cc)
// include that header, whose static init installs the check here; a TU
// without the interp simply has nothing to run. Hook and check both go
// away with the tree-walker in B7-f.
inline void (*&_canon_sig_check_hook())() {
  static void (*hook)() = nullptr;
  return hook;
}
inline void _check_canon_sigs_once() {
  if (auto* h = _canon_sig_check_hook()) h();
}

extern "C" {
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_namespace_get(const char* name,
                               int8_t* out_tag, int64_t* out_data) {
  _check_canon_sigs_once();
  std::string nm(name ? name : "");
  // Bare builtin function used as a value (`let f = inspect`, `map(type_of)`,
  // `assert_eq`). Checked before the namespace lookup since these names are
  // not namespaces; the culebra-source ones (matcher family, String helpers)
  // resolve the same way, through their group module.
  auto& table = culebra::runtime_substate<_JitNamespaceTable>(
      culebra::kSlotJitNamespaceTable);
  JitClosure* cls = _jit_builtin_fn_closure(table, nm);
  if (!cls) cls = _jit_lazy_fn_closure(table, nm);
  if (cls) {
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

// Record a lazy source module's builder fn_ptr under `name`. Emitted by the JIT
// splice as `_lazy_ns_register("Time", _time_module)` at entry-module top level;
// `builder` is the captureless `fn(){ <module body>; <Object> }` closure. Only
// its machine-code fn_ptr is kept (process-global, valid on any Runtime); the
// closure object itself is not retained. namespace_get rebuilds + invokes it
// per Runtime. See _jit_namespace_get_or_build.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_lazy_ns_register(const char* name, int8_t builder_tag,
                                  int64_t builder_data) {
  if (builder_tag != TAG_FUNC) return;  // splice only ever passes a closure
  auto* c = reinterpret_cast<JitClosure*>(builder_data);
  // A lane whose closures share one fn_ptr keeps the chunk in capture 0, so
  // that one does not count against the captureless rule below.
  JitCell* desc = _jit_closure_desc_hook ? _jit_closure_desc_hook(c) : nullptr;
  size_t n_own = c->n_captures - (desc ? 1 : 0);
  // A module builder closes over nothing (it references only builtins +
  // its own locals), so the per-Runtime rebuild uses 0 captures. A non-zero
  // count means the builder body accidentally referenced an entry-module
  // global (the free-var analysis's UFCS-candidate check can pull one in via
  // a call-form `x.name(...)` where `name` collides with a user global fn —
  // see the dynamic-perform cycle notes); the rebuilt closure would then read
  // a capture cell that was never populated, a release-silent null deref.
  // Fail loudly here instead, at the point the mistake was made.
  if (n_own != 0) {
    throw culebra::CulebraError(
        "InternalError",
        std::format("lazy-ns builder '{}' must be captureless (has {} "
                    "capture(s)) — a call-form method name in its body "
                    "likely collides with a user global fn",
                    name ? name : "?", n_own),
        0, 0);
  }
  _lazy_ns_register_builder(name ? name : "", c->fn_ptr,
                            desc ? desc->value.data : 0);
}

// Cold arm of jit.h's emit_reject_bare_builtin_method. The codegen filter
// (builtin method name + Nil read + no own slot) is receiver-blind, but a
// bare `x.map` is rejected only when the receiver's own builtin table would
// have dispatched it (the tree-walker's eval_property reject_if_bare rule);
// any other miss — class object, class instance, plain dict, range — reads
// as nil. Decide with the canonical tables so the lanes can never drift:
// throw the shared TypeError when the binder would dispatch, else return and
// the Nil read stands.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_bare_builtin_reject(
    int8_t tag, int64_t data, const char* key, int64_t line, int64_t col) {
  std::string_view name(key);
  bool would_dispatch = false;
  switch (tag) {
    case TAG_STRING:
    case TAG_STRINGVIEW:
      would_dispatch = culebra::canon_string_sigs().contains(name);
      break;
    case TAG_SET:
      would_dispatch = culebra::canon_set_sigs().contains(name);
      break;
    case TAG_TUPLE:
      would_dispatch = culebra::canon_tuple_sigs().contains(name);
      break;
    case TAG_ARRAY:
      would_dispatch = culebra::canon_array_sigs().contains(name);
      break;
    case TAG_TENSOR:
      would_dispatch = culebra::canon_tensor_sigs().contains(name);
      break;
    case TAG_OBJECT: {
      auto* obj = reinterpret_cast<JitObject*>(data);
      // Shared.new views bypass every reject site in the interp (misses read
      // the frozen tree, nil on absence) — mirror that.
      if (obj->is_shared_val) return;
      // A user property — own or proto, e.g. a getter that returned nil —
      // is first-class on both backends; never reject it.
      if (_find_property(obj, key)) return;
      // interp's ObjectValue::has(): own props ∪ the dict builtins. This arm
      // deliberately stays on the hand-written shared.h set rather than
      // canon_object_sigs(): the interp's own TAG_OBJECT reject site reads
      // is_object_builtin_method_name — not the builtins table — so lockstep
      // with it is what keeps the lanes symmetric. Unified in B7-f.
      auto has = [&](const char* k) {
        return _find_property(obj, k) != nullptr ||
               culebra::is_object_builtin_method_name(k);
      };
      would_dispatch =
          culebra::is_object_builtin_method_name(name) ||
          (has("next") && (has("has_next") || has("iter")) &&
           culebra::canon_iterator_sigs().contains(name));
      break;
    }
    default:
      return;  // scalar receivers dispatch no builtin table
  }
  if (would_dispatch) {
    throw culebra::CulebraError(
        "TypeError",
        std::format("built-in method '{}' cannot be used as a value "
                    "(call it, or wrap it in a lambda)",
                    name),
        line, col);
  }
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
  // arg0 check is inert here; pass the call site for it. This entry is a
  // direct-call peephole (never a UFCS site), so the boundary is the call
  // site too.
  return _jit_ns_method_dispatch(m, n_pos, positional, line, col, line, col,
                                 line, col);
}
}  // extern "C"

#ifdef CULEBRA_JIT_ENABLED
inline void JitExtension::declare_runtime(JIT& jit) {
  auto ptrTy = llvm::PointerType::get(jit.ctx_, 0);

  jit.module_->getOrInsertFunction(rt::print, jit.builder_.getVoidTy(),
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::println, jit.builder_.getVoidTy(),
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::to_long,
                               jit.builder_.getInt64Ty(), ptrTy,
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::to_long_any, jit.valueType_,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
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
                               jit.builder_.getVoidTy(), ptrTy,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::math_pow,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::math_wrap,
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
  // clamp(x, lo, hi): three (tag, data) pairs + line + col -> JitValue.
  jit.module_->getOrInsertFunction(rt::math_clamp, jit.valueType_,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
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
  jit.module_->getOrInsertFunction(rt::sys_getcwd, ptrTy,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::sys_chdir, jit.builder_.getVoidTy(),
                               ptrTy, jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::sys_set_env, jit.builder_.getVoidTy(),
                               ptrTy, ptrTy, jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
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
  // _Term (terminal-control primitives).
  jit.module_->getOrInsertFunction(rt::term_cols, i64);
  jit.module_->getOrInsertFunction(rt::term_rows, i64);
  jit.module_->getOrInsertFunction(rt::term_raw_on, jit.builder_.getVoidTy());
  jit.module_->getOrInsertFunction(rt::term_raw_off, jit.builder_.getVoidTy());
  jit.module_->getOrInsertFunction(rt::term_flush, jit.builder_.getVoidTy());
  jit.module_->getOrInsertFunction(rt::term_resized, jit.builder_.getInt1Ty());
  jit.module_->getOrInsertFunction(rt::term_width, i64, ptrTy);
  jit.module_->getOrInsertFunction(rt::term_color_level, i64);
  jit.module_->getOrInsertFunction(rt::term_read_key, ptrTy,
                                   jit.builder_.getDoubleTy());
  jit.module_->getOrInsertFunction(rt::term_attach_tty, jit.builder_.getInt1Ty());
  // _Canvas (immediate-mode 2D framebuffer primitives).
  auto vt = jit.builder_.getVoidTy();
  jit.module_->getOrInsertFunction(rt::canvas_coord, i64,
                                   jit.builder_.getDoubleTy());
  jit.module_->getOrInsertFunction(rt::canvas_init, vt, i64, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_clear, vt, i64);
  jit.module_->getOrInsertFunction(rt::canvas_set_pixel, vt, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_get_pixel, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_rect, vt, i64, i64, i64, i64, i64,
                                   i64);
  jit.module_->getOrInsertFunction(rt::canvas_line, vt, i64, i64, i64, i64,
                                   i64);
  jit.module_->getOrInsertFunction(rt::canvas_ellipse, vt, i64, i64, i64, i64,
                                   i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_triangle, vt, i64, i64, i64, i64,
                                   i64, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_polygon, vt, ptrTy, i64, i64,
                                   i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_font_load, i64, ptrTy);
  jit.module_->getOrInsertFunction(rt::canvas_glyph, vt, i64, i64, i64, i64,
                                   i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_sprite_load, i64, ptrTy, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_sprite_from_png, i64,
                                   jit.builder_.getInt8Ty(), i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_sprite_to_png, ptrTy, i64, i64,
                                   i64);
  jit.module_->getOrInsertFunction(rt::canvas_sprite_width, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_sprite_height, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_sprite_blank, i64, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_sprite_free, vt, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_ttf_load, i64,
                                   jit.builder_.getInt8Ty(), i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_ttf_free, vt, i64);
  jit.module_->getOrInsertFunction(rt::canvas_ttf_glyph, i64, i64, i64, i64,
                                   i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_ttf_glyph_screen, i64, i64, i64,
                                   i64, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_screen_width, i64);
  jit.module_->getOrInsertFunction(rt::canvas_screen_height, i64);
  jit.module_->getOrInsertFunction(rt::canvas_get_screen_pixel, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_screen_scale,
                                   jit.builder_.getDoubleTy());
  jit.module_->getOrInsertFunction(rt::canvas_ttf_advance, i64, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_ttf_ascent, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_target, i64, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_blit, vt, i64, i64, i64, i64, i64,
                                   i64, i64, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_blit_scaled, vt, i64, i64, i64,
                                   i64, i64, i64, i64, i64, i64, i64, i64, i64,
                                   i64);
  jit.module_->getOrInsertFunction(rt::canvas_present, vt, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_buttons, i64);
  jit.module_->getOrInsertFunction(rt::canvas_mouse_x, i64);
  jit.module_->getOrInsertFunction(rt::canvas_mouse_y, i64);
  jit.module_->getOrInsertFunction(rt::canvas_mouse_buttons, i64);
  jit.module_->getOrInsertFunction(rt::canvas_key, jit.builder_.getInt1Ty(),
                                   jit.builder_.getInt8Ty(), i64);
  jit.module_->getOrInsertFunction(rt::canvas_title, jit.builder_.getVoidTy(),
                                   jit.builder_.getInt8Ty(), i64);
  jit.module_->getOrInsertFunction(rt::canvas_key_pop, ptrTy);
  jit.module_->getOrInsertFunction(rt::canvas_char_pop, ptrTy);
  jit.module_->getOrInsertFunction(rt::canvas_closing, jit.builder_.getInt1Ty());
  jit.module_->getOrInsertFunction(rt::canvas_windowed,
                                   jit.builder_.getInt1Ty());
  jit.module_->getOrInsertFunction(rt::canvas_tone, vt, i64, i64, i64, i64, i64,
                                   i64, i64, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_music_play, vt,
                                   jit.builder_.getInt8Ty(), i64, i64, i64,
                                   jit.builder_.getDoubleTy(), i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_music_stop, vt);
  jit.module_->getOrInsertFunction(rt::canvas_music_pause, vt);
  jit.module_->getOrInsertFunction(rt::canvas_music_resume, vt);
  jit.module_->getOrInsertFunction(rt::canvas_music_volume, vt, i64);
  jit.module_->getOrInsertFunction(rt::canvas_music_seek, vt,
                                   jit.builder_.getDoubleTy());
  jit.module_->getOrInsertFunction(rt::canvas_music_playing,
                                   jit.builder_.getInt1Ty());
  jit.module_->getOrInsertFunction(rt::canvas_sound_load, i64,
                                   jit.builder_.getInt8Ty(), i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_sound_play, vt, i64, i64);
  jit.module_->getOrInsertFunction(rt::canvas_sound_stop, vt, i64);
  jit.module_->getOrInsertFunction(rt::canvas_sound_playing,
                                   jit.builder_.getInt1Ty(), i64);
  jit.module_->getOrInsertFunction(rt::canvas_sound_free, vt, i64);
  jit.module_->getOrInsertFunction(rt::canvas_width, i64);
  jit.module_->getOrInsertFunction(rt::canvas_height, i64);
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
  jit.module_->getOrInsertFunction(rt::tensor_concat, ptrTy, ptrTy,
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
  jit.module_->getOrInsertFunction(rt::tensor_item, jit.valueType_, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_no_grad, jit.valueType_, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_dot, ptrTy, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_from_csv, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_unary, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_linear_sigmoid, ptrTy,
                               ptrTy, ptrTy, ptrTy);
  // Autograd: all take a JitTensor* and return a JitTensor*.
  jit.module_->getOrInsertFunction(rt::tensor_requires_grad, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_grad, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_backward, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_zero_grad, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_detach, ptrTy, ptrTy);

  // Iterator terminal methods.
  auto i64 = jit.builder_.getInt64Ty();
  auto i8 = jit.builder_.getInt8Ty();
  jit.module_->getOrInsertFunction(rt::iter_collect, ptrTy, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_join, ptrTy, i8, i64, ptrTy);
  jit.module_->getOrInsertFunction(rt::iter_count, i64, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_for_each, jit.builder_.getVoidTy(),
                               i8, i64, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_reduce, jit.builder_.getVoidTy(),
                               i8, i64, i8, i64, i8, i64, i64, i64,
                               ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::iter_find, jit.builder_.getVoidTy(),
                               i8, i64, i8, i64, i64, i64, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::iter_position, jit.builder_.getVoidTy(),
                               i8, i64, i8, i64, i64, i64, ptrTy, ptrTy);
  // contains borrows the needle, so it needs no position for an error.
  jit.module_->getOrInsertFunction(rt::iter_contains, i64, i8, i64, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_first, jit.builder_.getVoidTy(),
                               i8, i64, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::iter_last, jit.builder_.getVoidTy(),
                               i8, i64, ptrTy, ptrTy);
  // nth carries line+col for the "n must not be negative" error.
  jit.module_->getOrInsertFunction(rt::iter_nth, jit.builder_.getVoidTy(),
                               i8, i64, i64, i64, i64, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::iter_any, i64, i8, i64, i8, i64, i64,
                               i64);
  // sum/product/min/max: (it, id, line, col) -> %Value (see the Array decls
  // in jit.h — the result tag is data-dependent).
  jit.module_->getOrInsertFunction(rt::iter_sum, jit.valueType_, i8, i64, i64,
                               i64);
  jit.module_->getOrInsertFunction(rt::iter_product, jit.valueType_, i8, i64,
                               i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_min, jit.valueType_, i8, i64, i64,
                               i64);
  jit.module_->getOrInsertFunction(rt::iter_max, jit.valueType_, i8, i64, i64,
                               i64);
  // min_by/max_by: (it, id, ft, fd, line, col) -> %Value
  jit.module_->getOrInsertFunction(rt::iter_min_by, jit.valueType_, i8, i64,
                               i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_max_by, jit.valueType_, i8, i64,
                               i8, i64, i64, i64);
  // to_set/to_object/group_by/partition return a fresh Set / Object /
  // Object / Tuple pointer.
  jit.module_->getOrInsertFunction(rt::iter_to_set, ptrTy, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_to_object, ptrTy, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_group_by, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_partition, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_unzip, ptrTy, i8, i64, i64, i64);
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
  jit.module_->getOrInsertFunction(rt::iter_skip_while, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  // flatten/distinct carry line+col for the not-iterable / unhashable errors.
  jit.module_->getOrInsertFunction(rt::iter_flatten, ptrTy, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_distinct, ptrTy, i8, i64, i64, i64);
  // scan takes the seed as a value pair before the callback.
  jit.module_->getOrInsertFunction(rt::iter_scan, ptrTy, i8, i64, i8, i64, i8,
                               i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_tap, ptrTy, i8, i64, i8, i64, i64,
                               i64);
  jit.module_->getOrInsertFunction(rt::iter_chunk_by, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  // step_by carries line+col for the "n must be at least 1" error.
  jit.module_->getOrInsertFunction(rt::iter_step_by, ptrTy, i8, i64, i64, i64,
                               i64);
  // chunks/windows carry line+col for the "n must be at least 1" error.
  jit.module_->getOrInsertFunction(rt::iter_chunks, ptrTy, i8, i64, i64, i64,
                               i64);
  jit.module_->getOrInsertFunction(rt::iter_windows, ptrTy, i8, i64, i64, i64,
                               i64);
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
  jit.module_->getOrInsertFunction(rt::str_bytes, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::array_iter, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::object_iter, ptrTy, ptrTy);

  // JSON: stringify takes (tag, data, indent, sort_keys, lines) ->
  // String; parse takes (String, number_mode_cstr, lines, jsonc) -> Value.
  jit.module_->getOrInsertFunction(rt::json_stringify, ptrTy,
                                jit.builder_.getInt8Ty(),
                                jit.builder_.getInt64Ty(),
                                jit.builder_.getInt64Ty(),
                                jit.builder_.getInt8Ty(),
                                jit.builder_.getInt8Ty());
  jit.module_->getOrInsertFunction(rt::json_parse, jit.valueType_, ptrTy,
                                ptrTy, jit.builder_.getInt8Ty(),
                                jit.builder_.getInt8Ty());
}
#endif  // CULEBRA_JIT_ENABLED

inline bool JitExtension::is_builtin_var(const std::string& name) {
  static const std::unordered_set<std::string_view> names = {
      "inspect", "print",   "println",   "repeat",
      "to_long", "to_float",  "to_string", "type_of", "hash", "__eff_copy",
      "__eff_abort", "__eff_catch_abort",
      "Math",    "IO",        "FS",        "File",     "Embed",   "_Time",
      "Random",  "Sys",       "JSON",      "Tensor",   "GC",
      "_Regex",  "Proc",      "Net",       "Isolate",   "Channel",  "Parallel",
      "Signal",  "Encoding", "Compress",  "SharedBuffer", "Shared",
      "Hash",    "CSV",       "TOML",      "Env",       "UUID",       "String",
      "_Term",   "_Canvas",
#if defined(CULEBRA_SQLITE_ENABLED)
      "SQLite",
#endif
      // Lazy source modules (preamble builders, resolved via namespace_get +
      // the lazy-ns builder registry). Listed here so closures capture-skip
      // them and bare references compile to namespace_get — mirroring the
      // interp's builtin_names skip. See _jit_namespace_get_or_build.
      "Time",    "Args",      "Regex",     "Term",      "Log",      "Path",
      "Canvas",  "__Eff",     "Vector2",   "Vector3",
      // The bare function globals from those same source modules (assert_*,
      // `replace`) are listed by lazy_fn_group_of below, not here.
#if defined(CULEBRA_HTTP_ENABLED)
      "Http",
#endif
#if defined(CULEBRA_ENABLE_WEBVIEW)
      "Desktop",
#endif
  };
  if (names.contains(name)) return true;
  if (!culebra::lazy_fn_group_of(name).empty()) return true;
  // wrap.h-declared namespaces (e.g. __Foreign) — registry-driven.
  for (const auto& m : _wrapped_ns_methods()) {
    if (name == m.ns) return true;
  }
  return false;
}


}  // namespace culebra
