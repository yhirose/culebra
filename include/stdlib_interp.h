#pragma once

// Interpreter-side implementation of the Culebra standard library.
//
// Core built-ins bound on every environment: to_long, to_float,
// to_string, type_of (see docs/language.md §18). Everything else is
// grouped under a namespace ObjectValue: Math (abs/min/max/pow/sign/
// clamp/iota), IO (inspect/print/println/input/read/write), Sys
// (argv/exit/env). The CLI (src/main.cc) additionally aliases
// IO.inspect / IO.print / IO.println as globals for scripting
// ergonomics; embedders get a clean environment by default.
//
// Independent header. Include from main.cc (or any embedder) after
// interpreter.h to wire stdlib into a fresh Environment via
// `culebra::environment()` or `culebra::setup_built_in_functions(env)`.

#include <compress.h>
#include <image.h>
#include <csv.h>
#include <env.h>
#include <foreign.h>
#include <foreign_binding.h>
#include <hash.h>
#include <json.h>
#if defined(CULEBRA_SQLITE_ENABLED)
#include <sqlite.h>
#endif
#include <toml.h>
#include <uuid.h>
#include <vfs.h>
#include <interpreter.h>
#include <net.h>
#include <proc.h>
#if defined(CULEBRA_HTTP_ENABLED)
#include <http.h>
#endif
#include <canvas.h>
#include <regexlib.h>
#include <term.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <numbers>  // std::numbers::pi / ::e (Math.pi / Math.e; portable vs M_PI)
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <limits>
#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <os_compat.h>  // os_isatty / os_setenv (isatty / setenv shims)
// POSIX ownership (chown / getpwnam / st_uid) has no Windows equivalent; FS.chown
// and the FS.stat owner fields are stubbed on Windows (see the _WIN32 branches
// below), so these headers are POSIX-only.
#if !defined(_WIN32)
#include <grp.h>     // getgrnam_r (FS.chown group-name resolution)
#include <pwd.h>     // getpwnam_r (FS.chown user-name resolution)
#include <sys/stat.h>  // ::stat for st_uid/st_gid (FS.stat owner fields)
#include <unistd.h>  // chown (FS.chown)
#endif
#include <vector>

namespace culebra {

inline Value make_math_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  ns.initialize(
      "abs",
      Value(FunctionValue({{"x", false}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& x = env->get("x");
                            if (x.type == Value::Long) {
                              auto v = x.get<int64_t>();
                              return Value(v < 0 ? -v : v);
                            }
                            if (x.type == Value::Float) {
                              return Value(std::fabs(x.get<double>()));
                            }
                            auto line = env->get("__LINE__").to_long();
                            auto col = env->get("__COLUMN__").to_long();
                            throw_type_error_at(line, col);
                          })),
      false);

  // Returns Long if every arg was Long, else Float. Requires ≥2 args.
  auto numeric_reduce = [](std::shared_ptr<Environment> env,
                           auto better) {
    int64_t line = env->get("__LINE__").to_long();
    int64_t col = env->get("__COLUMN__").to_long();
    if (!env->has("__ARGS__")) throw_type_error_at(line, col);
    const auto& extras = *env->get("__ARGS__").to_array().values;
    if (extras.empty()) throw_type_error_at(line, col);
    bool any_float = false;
    for (const auto& v : extras) {
      if (!v.is_numeric()) throw_type_error_at(line, col);
      if (v.type == Value::Float) any_float = true;
    }
    if (any_float) {
      double acc = extras[0].to_double_coerce();
      for (size_t i = 1; i < extras.size(); i++) {
        double x = extras[i].to_double_coerce();
        if (better(x, acc)) acc = x;
      }
      return Value(acc);
    }
    int64_t acc = extras[0].get<int64_t>();
    for (size_t i = 1; i < extras.size(); i++) {
      int64_t x = extras[i].get<int64_t>();
      if (better(static_cast<double>(x), static_cast<double>(acc))) acc = x;
    }
    return Value(acc);
  };

  // Modeled as `min(*args)` / `max(*args)`: the >=1 positional args are read
  // from __ARGS__ by numeric_reduce. Declaring `*args` (rather than an empty
  // list) makes the arity variadic so they stay usable as higher-order
  // callbacks (`map(Math.max)`), matching the JIT — mirrors range/iota.
  ns.initialize(
      "min",
      Value(FunctionValue({FunctionValue::Parameter::make_args_rest("args")},
                          [numeric_reduce](std::shared_ptr<Environment> env) {
        return numeric_reduce(env, [](double a, double b) { return a < b; });
      })),
      false);

  ns.initialize(
      "max",
      Value(FunctionValue({FunctionValue::Parameter::make_args_rest("args")},
                          [numeric_reduce](std::shared_ptr<Environment> env) {
        return numeric_reduce(env, [](double a, double b) { return a > b; });
      })),
      false);

  // Integer power: 0^0 == 1; negative exponent throws.
  ns.initialize(
      "pow",
      Value(FunctionValue({{"base", false, "Long"sv}, {"exp", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto base = env->get("base").to_long();
                            auto exp = env->get("exp").to_long();
                            if (exp < 0) {
                              auto line = env->get("__LINE__").to_long();
                              auto col = env->get("__COLUMN__").to_long();
                              throw_type_error_at(line, col);
                            }
                            return Value(ipow_nonneg(base, exp));
                          },
                          "Long"sv)),
      false);

  ns.initialize(
      "sign",
      Value(FunctionValue({{"x", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto x = env->get("x").to_long();
                            return Value(int64_t{x > 0 ? 1 : (x < 0 ? -1 : 0)});
                          },
                          "Long"sv)),
      false);

  ns.initialize(
      "clamp",
      Value(FunctionValue({{"x", false, "Long"sv},
                           {"lo", false, "Long"sv},
                           {"hi", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto x = env->get("x").to_long();
                            auto lo = env->get("lo").to_long();
                            auto hi = env->get("hi").to_long();
                            if (x < lo) return Value(lo);
                            if (x > hi) return Value(hi);
                            return Value(x);
                          },
                          "Long"sv)),
      false);

  ns.initialize(
      "wrap",
      Value(FunctionValue({{"x", false, "Long"sv}, {"n", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto x = env->get("x").to_long();
                            auto n = env->get("n").to_long();
                            if (n == 0) {
                              auto line = env->get("__LINE__").to_long();
                              auto col = env->get("__COLUMN__").to_long();
                              throw CulebraError("ZeroDivisionError",
                                                 "divide by 0 error", line, col);
                            }
                            return Value(floored_mod(x, n));
                          },
                          "Long"sv)),
      false);

  auto float_to_float = [](auto fn) {
    return Value(FunctionValue(
        {{"x", false}},
        [fn](std::shared_ptr<Environment> env) {
          const auto& v = env->get("x");
          if (!v.is_numeric()) {
            auto line = env->get("__LINE__").to_long();
            auto col = env->get("__COLUMN__").to_long();
            throw_type_error_at(line, col);
          }
          return Value(fn(v.to_double_coerce()));
        },
        "Float"sv));
  };
  auto float_to_long = [](auto fn) {
    return Value(FunctionValue(
        {{"x", false}},
        [fn](std::shared_ptr<Environment> env) {
          const auto& v = env->get("x");
          if (v.type == Value::Long) return v;
          if (v.type != Value::Float) {
            auto line = env->get("__LINE__").to_long();
            auto col = env->get("__COLUMN__").to_long();
            throw_type_error_at(line, col);
          }
          return Value(static_cast<int64_t>(fn(v.get<double>())));
        },
        "Long"sv));
  };
  ns.initialize("log",   float_to_float([](double x) { return std::log(x); }), false);
  ns.initialize("exp",   float_to_float([](double x) { return std::exp(x); }), false);
  ns.initialize("sqrt",  float_to_float([](double x) { return std::sqrt(x); }), false);
  ns.initialize("sin",   float_to_float([](double x) { return std::sin(x); }), false);
  ns.initialize("cos",   float_to_float([](double x) { return std::cos(x); }), false);
  ns.initialize("tan",   float_to_float([](double x) { return std::tan(x); }), false);
  ns.initialize("asin",  float_to_float([](double x) { return std::asin(x); }), false);
  ns.initialize("acos",  float_to_float([](double x) { return std::acos(x); }), false);
  ns.initialize("atan",  float_to_float([](double x) { return std::atan(x); }), false);
  // atan2(y, x): two numeric args -> Float (radians), shared shape with the
  // JIT's culebra_runtime_math_atan2.
  ns.initialize(
      "atan2",
      Value(FunctionValue(
          {{"y", false}, {"x", false}},
          [](std::shared_ptr<Environment> env) {
            const auto& y = env->get("y");
            const auto& x = env->get("x");
            if (!y.is_numeric() || !x.is_numeric()) {
              auto line = env->get("__LINE__").to_long();
              auto col = env->get("__COLUMN__").to_long();
              throw_type_error_at(line, col);
            }
            return Value(std::atan2(y.to_double_coerce(), x.to_double_coerce()));
          },
          "Float"sv)),
      false);
  ns.initialize("floor", float_to_long ([](double x) { return std::floor(x); }), false);
  ns.initialize("ceil",  float_to_long ([](double x) { return std::ceil(x); }), false);
  // std::rint honors the current IEEE 754 rounding mode, which defaults to
  // round-half-to-even (banker's rounding, matching Python's built-in round()).
  ns.initialize("round", float_to_long ([](double x) { return std::rint(x); }), false);

  ns.initialize("pi",  Value(std::numbers::pi), false);
  ns.initialize("e",   Value(std::numbers::e), false);
  ns.initialize("inf", Value(std::numeric_limits<double>::infinity()), false);
  ns.initialize("nan", Value(std::numeric_limits<double>::quiet_NaN()), false);

  return Value(std::move(ns));
}

// `IO.stdin()` -> a read-only handle over standard input, sharing the File
// reader shape: `.lines()` (lazy, newline-stripped) and `.read(n=nil)` (nil =
// rest of input, Long = up to n bytes). No close/seek (stdin isn't seekable),
// so source-generic code that only reads works over both File and stdin.
inline Value make_stdin_handle() {
  using namespace std::literals;
  ObjectValue h;
  // Native methods aren't Sendable; reject at the isolate boundary like File.
  h.initialize("__nonsendable__", Value(true), false);

  h.initialize(
      "read",
      Value(FunctionValue({{"n", false, ""sv, nullptr, kw_default_nil()}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            const auto& n = env->get("n");
                            if (n.type == Value::Nil)
                              return Value(read_stdin_all_interruptible());
                            return Value(read_stdin_n_interruptible(
                                static_cast<size_t>(n.to_long())));
                          },
                          "String"sv)),
      false);

  h.initialize(
      "lines",
      Value(FunctionValue({},
                          [](std::shared_ptr<Environment>) {
                            return _make_iterator(
                                [](std::shared_ptr<Environment>)
                                    -> std::optional<Value> {
                                  std::string out;
                                  if (!read_stdin_line_interruptible(out))
                                    return _iter_step_done();
                                  return _iter_step_value(Value(std::move(out)));
                                });
                          },
                          "Object"sv)),
      false);

  return Value(std::move(h));
}

inline Value make_io_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  ns.initialize("inspect",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      auto s = str_quoted_with_special(
                                          env->get("arg"));
                                      std::lock_guard<std::mutex> lk(
                                          stdio_mutex());
                                      std::cout << s << std::endl;
                                      return Value();
                                    })),
                false);

  ns.initialize("print",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      auto s = str_display_with_special(
                                          env->get("arg"));
                                      std::lock_guard<std::mutex> lk(
                                          stdio_mutex());
                                      std::cout << s;
                                      return Value();
                                    })),
                false);

  ns.initialize("println",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      auto s = str_display_with_special(
                                          env->get("arg"));
                                      std::lock_guard<std::mutex> lk(
                                          stdio_mutex());
                                      std::cout << s << std::endl;
                                      return Value();
                                    })),
                false);

  ns.initialize(
      "input",
      Value(FunctionValue({},
                          [](std::shared_ptr<Environment>) {
                            std::string line;
                            // Interruptible; empty string at EOF (unchanged).
                            read_stdin_line_interruptible(line);
                            return Value(std::move(line));
                          },
                          "String"sv)),
      false);

  // `IO.stdin()` — a read-only handle over standard input. Use `.read()` for
  // the whole stream (the portable replacement for `FS.read("/dev/stdin")`),
  // `.read(n)` for n bytes, or `.lines()` to stream line by line. No
  // stdio_mutex: a blocking, interruptible stdin read held under it would
  // stall every isolate's output, and stdin is single-consumer.
  ns.initialize(
      "stdin",
      Value(FunctionValue({},
                          [](std::shared_ptr<Environment>) {
                            return make_stdin_handle();
                          },
                          "Object"sv)),
      false);

  // Write to standard error — the twin of print / inspect (no stderr writer
  // existed before). einspect quotes + newline like inspect; eprint is raw;
  // eprintln is raw + newline.
  ns.initialize("einspect",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      auto s = str_quoted_with_special(
                                          env->get("arg"));
                                      std::lock_guard<std::mutex> lk(
                                          stdio_mutex());
                                      std::cerr << s << std::endl;
                                      return Value();
                                    })),
                false);
  ns.initialize("eprint",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      auto s = str_display_with_special(
                                          env->get("arg"));
                                      std::lock_guard<std::mutex> lk(
                                          stdio_mutex());
                                      std::cerr << s;
                                      return Value();
                                    })),
                false);
  ns.initialize("eprintln",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      auto s = str_display_with_special(
                                          env->get("arg"));
                                      std::lock_guard<std::mutex> lk(
                                          stdio_mutex());
                                      std::cerr << s << std::endl;
                                      return Value();
                                    })),
                false);

  // Terminal detection (POSIX isatty) per standard stream. Lets a script
  // branch on interactivity: prompt vs read a pipe (stdin), colorize vs emit
  // plain output (stdout/stderr). Mirrors Rust `io::stdin().is_terminal()` /
  // Node `process.stdin.isTTY`.
  auto is_terminal = [](int fd) {
    return Value(FunctionValue(
        {}, [fd](std::shared_ptr<Environment>) { return Value(os_isatty(fd)); },
        "Bool"sv));
  };
  ns.initialize("stdin_is_terminal", is_terminal(0), false);
  ns.initialize("stdout_is_terminal", is_terminal(1), false);
  ns.initialize("stderr_is_terminal", is_terminal(2), false);

  // File I/O lives on FS (FS.read / FS.write / FS.exists). IO is the
  // standard-stream + console namespace: inspect / print / println / input.
  return Value(std::move(ns));
}

// Opt-in: alias IO.inspect / IO.print / IO.println as bare globals (CLI and
// playground scripting ergonomics). Embedders get a clean environment unless
// called.
inline void install_cli_aliases(Environment& env) {
  const auto& io = env.get("IO").to_object();
  env.initialize("inspect", io.get("inspect"), false);
  env.initialize("print", io.get("print"), false);
  env.initialize("println", io.get("println"), false);
}

// --- Glob (file-scope, shared between interp + JIT) ---

namespace _glob_detail {

// Match a single path component against a glob token supporting `*`, `?`,
// and `[...]` character classes. Backtracking on `*`.
inline bool match_segment(std::string_view pat, std::string_view name) {
  size_t pi = 0, ni = 0, star = std::string_view::npos, mark = 0;
  while (ni < name.size()) {
    if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == name[ni])) {
      ++pi; ++ni;
    } else if (pi < pat.size() && pat[pi] == '[') {
      size_t close = pat.find(']', pi + 1);
      if (close == std::string_view::npos) return false;
      auto cls = pat.substr(pi + 1, close - pi - 1);
      bool neg = !cls.empty() && (cls[0] == '!' || cls[0] == '^');
      if (neg) cls.remove_prefix(1);
      bool hit = false;
      for (size_t k = 0; k < cls.size(); ++k) {
        if (k + 2 < cls.size() && cls[k + 1] == '-') {
          if (name[ni] >= cls[k] && name[ni] <= cls[k + 2]) hit = true;
          k += 2;
        } else if (cls[k] == name[ni]) {
          hit = true;
        }
      }
      if (hit == neg) return false;
      pi = close + 1; ++ni;
    } else if (pi < pat.size() && pat[pi] == '*') {
      star = pi++; mark = ni;
    } else if (star != std::string_view::npos) {
      pi = star + 1; ni = ++mark;
    } else {
      return false;
    }
  }
  while (pi < pat.size() && pat[pi] == '*') ++pi;
  return pi == pat.size();
}

// Recursively expand glob segments starting at directory `base`.
inline void expand(const std::filesystem::path& base,
                   const std::vector<std::string>& segs, size_t si,
                   std::vector<std::string>& out) {
  if (si == segs.size()) {
    if (!base.empty()) out.push_back(base.string());
    return;
  }
  const auto& seg = segs[si];
  if (seg == "**") {
    // `**` matches zero or more directories: try skipping it, and try
    // descending into every subdir while keeping `**` active.
    expand(base, segs, si + 1, out);
    std::error_code ec;
    std::filesystem::directory_iterator it(
        base.empty() ? std::filesystem::path(".") : base, ec);
    if (ec) return;
    for (const auto& e : it) {
      if (e.is_directory(ec)) expand(e.path(), segs, si, out);
    }
    return;
  }
  bool literal = seg.find_first_of("*?[") == std::string::npos;
  if (literal) {
    auto next = base.empty() ? std::filesystem::path(seg) : base / seg;
    std::error_code ec;
    if (std::filesystem::exists(next, ec)) expand(next, segs, si + 1, out);
    return;
  }
  std::error_code ec;
  std::filesystem::directory_iterator it(
      base.empty() ? std::filesystem::path(".") : base, ec);
  if (ec) return;
  for (const auto& e : it) {
    auto name = e.path().filename().string();
    if (match_segment(seg, name)) expand(e.path(), segs, si + 1, out);
  }
}

}  // namespace _glob_detail

inline std::vector<std::string> _fs_glob(std::string_view pattern) {
  std::vector<std::string> segs;
  bool absolute = !pattern.empty() && pattern[0] == '/';
  size_t i = 0;
  while (i < pattern.size()) {
    size_t j = pattern.find('/', i);
    if (j == std::string_view::npos) j = pattern.size();
    if (j > i) segs.emplace_back(pattern.substr(i, j - i));
    i = j + 1;
  }
  std::vector<std::string> out;
  _glob_detail::expand(absolute ? std::filesystem::path("/")
                                : std::filesystem::path(),
                       segs, 0, out);
  std::sort(out.begin(), out.end());
  return out;
}

// Last-write time of `p` as seconds since the Unix epoch, or 0 on error.
// Shared interp/JIT (file_time_type -> system_clock conversion for FS.stat).
inline int64_t _fs_mtime_secs(const std::filesystem::path& p) {
  std::error_code ec;
  auto ftime = std::filesystem::last_write_time(p, ec);
  if (ec) return 0;
  auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - std::filesystem::file_time_type::clock::now() +
      std::chrono::system_clock::now());
  return static_cast<long>(
      std::chrono::duration_cast<std::chrono::seconds>(
          sys.time_since_epoch()).count());
}

// Raise an IOError as `<what>: <ec.message()>.` (or just `<what>` when no
// error_code). The interp twin of the JIT's `_fs_throw_io` — shared by the FS
// and Sys namespaces so a failed syscall reports identically on both backends.
[[noreturn]] inline void _io_throw(const std::string& what, int64_t line, int64_t col,
                                   const std::error_code& ec = {}) {
  auto msg = ec ? std::format("{}: {}.", what, ec.message())
                : std::string(what);
  throw CulebraError("IOError", std::move(msg), line, col);
}

// --- FS ownership helpers (shared by interp + JIT/AOT so the three backends
// resolve names and report errors identically) ----------------------------

#if !defined(_WIN32)

// Read st_uid / st_gid of `p` (following symlinks, like the other stat fields).
// Returns false if the path can't be stat'd.
inline bool _fs_owner(const std::filesystem::path& p, int64_t& uid, int64_t& gid) {
  struct ::stat st;
  if (::stat(p.c_str(), &st) != 0) return false;
  uid = static_cast<long>(st.st_uid);
  gid = static_cast<long>(st.st_gid);
  return true;
}

// Resolve a user name to a uid (thread-safe getpwnam_r), or -1 if unknown.
inline int64_t _fs_uid_from_name(const std::string& name) {
  struct ::passwd pw;
  struct ::passwd* result = nullptr;
  std::vector<char> buf(1024);
  while (::getpwnam_r(name.c_str(), &pw, buf.data(), buf.size(), &result) ==
         ERANGE) {
    buf.resize(buf.size() * 2);
  }
  return result ? static_cast<long>(result->pw_uid) : -1;
}

// Resolve a group name to a gid (thread-safe getgrnam_r), or -1 if unknown.
inline int64_t _fs_gid_from_name(const std::string& name) {
  struct ::group gr;
  struct ::group* result = nullptr;
  std::vector<char> buf(1024);
  while (::getgrnam_r(name.c_str(), &gr, buf.data(), buf.size(), &result) ==
         ERANGE) {
    buf.resize(buf.size() * 2);
  }
  return result ? static_cast<long>(result->gr_gid) : -1;
}

// POSIX chown with uid/gid == -1 meaning "leave unchanged". Throws IOError.
inline void _fs_do_chown(const std::filesystem::path& p, int64_t uid, int64_t gid,
                         int64_t line, int64_t col) {
  if (::chown(p.c_str(), static_cast<uid_t>(uid),
              static_cast<gid_t>(gid)) != 0) {
    _io_throw(std::format("FS.chown('{}')", p.string()), line, col,
              std::error_code(errno, std::generic_category()));
  }
}

#else  // _WIN32 — POSIX numeric uid/gid ownership has no Windows equivalent.
       // FS.stat omits the owner fields (no uid/gid) and FS.chown raises.

inline bool _fs_owner(const std::filesystem::path&, int64_t&, int64_t&) {
  return false;  // no POSIX owner ids on Windows → stat omits uid/gid
}
inline int64_t _fs_uid_from_name(const std::string&) { return -1; }
inline int64_t _fs_gid_from_name(const std::string&) { return -1; }
inline void _fs_do_chown(const std::filesystem::path&, int64_t, int64_t,
                         int64_t line,
                         int64_t col) {
  throw CulebraError("RuntimeError",
                     "FS.chown is not supported on Windows (no POSIX uid/gid "
                     "ownership model)",
                     line, col);
}

#endif  // !_WIN32

inline Value make_fs_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  auto throw_io = [](const std::string& what, int64_t line, int64_t col,
                     const std::error_code& ec = {}) {
    _io_throw(what, line, col, ec);
  };

  // `FS.join(parts...)` — varargs path concat. Empty arg list -> "". Declared
  // `*args` (the positionals are read from __ARGS__) so the arity is variadic;
  // an empty formal list would now read as a strict 0-arity native.
  ns.initialize(
      "join",
      Value(FunctionValue({FunctionValue::Parameter::make_args_rest("args")},
                          [throw_io](std::shared_ptr<Environment> env) {
        int64_t line = env->get("__LINE__").to_long();
        int64_t col = env->get("__COLUMN__").to_long();
        if (!env->has("__ARGS__")) return Value(std::string(""));
        const auto& args = *env->get("__ARGS__").to_array().values;
        std::filesystem::path out;
        for (const auto& v : args) {
          out /= _fspath(v);  // String or Path component
        }
        return Value(out.string());
      })),
      false);

  auto string_to_string = [](auto fn) {
    return Value(FunctionValue(
        {{"path", false, "String|Path"sv}},
        [fn](std::shared_ptr<Environment> env) {
          auto p = _fspath(env->get("path"));
          return Value(fn(std::filesystem::path(p)));
        },
        "String"sv));
  };

  ns.initialize("basename",  string_to_string([](const std::filesystem::path& p) { return p.filename().string(); }), false);
  ns.initialize("dirname",   string_to_string([](const std::filesystem::path& p) { return p.parent_path().string(); }), false);
  ns.initialize("extension", string_to_string([](const std::filesystem::path& p) { return p.extension().string(); }), false);
  ns.initialize("stem",      string_to_string([](const std::filesystem::path& p) { return p.stem().string(); }), false);

  auto path_query_bool = [](auto fn) {
    return Value(FunctionValue(
        {{"path", false, "String|Path"sv}},
        [fn](std::shared_ptr<Environment> env) {
          auto p = _fspath(env->get("path"));
          std::error_code ec;
          return Value(fn(std::filesystem::path(p), ec));
        },
        "Bool"sv));
  };

  ns.initialize("exists",  path_query_bool([](const std::filesystem::path& p, std::error_code& ec) { return std::filesystem::exists(p, ec); }), false);
  ns.initialize("is_file", path_query_bool([](const std::filesystem::path& p, std::error_code& ec) { return std::filesystem::is_regular_file(p, ec); }), false);
  ns.initialize("is_dir",  path_query_bool([](const std::filesystem::path& p, std::error_code& ec) { return std::filesystem::is_directory(p, ec); }), false);

  // Whole-file read/write convenience (open + read/write + close in one
  // call). Streaming lives on the File handle. Always binary (raw bytes);
  // String is a byte string, so this round-trips arbitrary content.
  ns.initialize(
      "read",
      Value(FunctionValue({{"path", false, "String|Path"sv}},
                          [throw_io](std::shared_ptr<Environment> env) {
                            int64_t line = env->get("__LINE__").to_long();
                            int64_t col = env->get("__COLUMN__").to_long();
                            const auto& p = _fspath(env->get("path"));
                            std::ifstream ifs(p, std::ios::binary);
                            if (!ifs) {
                              throw_io(
                                  std::format("FS.read: cannot open '{}'", p),
                                  line, col);
                            }
                            std::string s(
                                (std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
                            return Value(std::move(s));
                          },
                          "String"sv)),
      false);

  ns.initialize(
      "write",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv}, {"content", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& p = _fspath(env->get("path"));
            const auto& c = env->get("content").to_string();
            std::ofstream ofs(p, std::ios::binary);
            if (!ofs) {
              throw_io(std::format("FS.write: cannot open '{}'", p), line, col);
            }
            ofs.write(c.data(), c.size());
            return Value();
          })),
      false);

  ns.initialize(
      "size",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& p = _fspath(env->get("path"));
            std::error_code ec;
            auto sz = std::filesystem::file_size(p, ec);
            if (ec) throw_io(std::format("FS.size('{}')", p), line, col, ec);
            return Value(static_cast<int64_t>(sz));
          },
          "Long"sv)),
      false);

  ns.initialize(
      "list_dir",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv}},
          [throw_io](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& p = _fspath(env->get("path"));
            std::error_code ec;
            auto it = std::filesystem::directory_iterator(p, ec);
            if (ec) throw_io(std::format("FS.list_dir('{}')", p), line, col, ec);
            ArrayValue av;
            for (const auto& entry : it) {
              av.values->emplace_back(entry.path().filename().string());
            }
            return Value(std::move(av));
          },
          "Array"sv)),
      false);

  ns.initialize(
      "mkdir",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& p = _fspath(env->get("path"));
            std::error_code ec;
            std::filesystem::create_directories(p, ec);
            if (ec) throw_io(std::format("FS.mkdir('{}')", p), line, col, ec);
            return Value();
          })),
      false);

  ns.initialize(
      "remove",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv},
           {"recursive", false, ""sv, nullptr, kw_default_false()}},
          [throw_io](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& p = _fspath(env->get("path"));
            bool recursive = env->get("recursive").to_bool();
            std::error_code ec;
            if (recursive) {
              if (std::filesystem::remove_all(p, ec) ==
                      static_cast<std::uintmax_t>(-1) || ec) {
                throw_io(std::format("FS.remove('{}', recursive: true)", p),
                         line, col, ec);
              }
            } else if (!std::filesystem::remove(p, ec) || ec) {
              throw_io(std::format("FS.remove('{}')", p), line, col, ec);
            }
            return Value();
          })),
      false);

  // `FS.stat(path)` -> Object{size, is_dir, is_file, is_symlink, mtime, mode,
  // uid, gid}. mtime is seconds since the Unix epoch (Long). IOError on
  // missing path.
  ns.initialize(
      "stat",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv}},
          [throw_io](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& p = _fspath(env->get("path"));
            std::error_code ec;
            auto st = std::filesystem::symlink_status(p, ec);
            if (ec || st.type() == std::filesystem::file_type::not_found) {
              throw_io(std::format("FS.stat('{}')", p), line, col, ec);
            }
            bool is_link =
                st.type() == std::filesystem::file_type::symlink;
            // Follow the link for size/dir/file/mtime.
            auto fst = std::filesystem::status(p, ec);
            std::uintmax_t sz = 0;
            if (std::filesystem::is_regular_file(fst)) {
              sz = std::filesystem::file_size(p, ec);
              if (ec) sz = 0;
            }
            int64_t mtime = _fs_mtime_secs(p);
            ObjectValue obj;
            obj.initialize("size", Value(static_cast<int64_t>(sz)), false);
            obj.initialize("is_dir",
                           Value(std::filesystem::is_directory(fst)), false);
            obj.initialize("is_file",
                           Value(std::filesystem::is_regular_file(fst)),
                           false);
            obj.initialize("is_symlink", Value(is_link), false);
            obj.initialize("mtime", Value(mtime), false);
            obj.initialize(
                "mode",
                Value(static_cast<int64_t>(fst.permissions() &
                                        std::filesystem::perms::mask)),
                false);
            int64_t uid = -1, gid = -1;
            _fs_owner(p, uid, gid);
            obj.initialize("uid", Value(uid), false);
            obj.initialize("gid", Value(gid), false);
            return Value(std::move(obj));
          },
          "Object"sv)),
      false);

  // `FS.chmod(path, mode)` — set permission bits, e.g. `0o755`. `mode` is
  // masked to the low 12 bits (rwx + setuid/setgid/sticky). IOError if the
  // path is missing or the permissions can't be changed.
  ns.initialize(
      "chmod",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv}, {"mode", false, "Long"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& p = _fspath(env->get("path"));
            int64_t mode = env->get("mode").to_long();
            std::error_code ec;
            std::filesystem::permissions(
                std::filesystem::path(p),
                static_cast<std::filesystem::perms>(
                    mode & static_cast<long>(std::filesystem::perms::mask)),
                std::filesystem::perm_options::replace, ec);
            if (ec) throw_io(std::format("FS.chmod('{}')", p), line, col, ec);
            return Value();
          })),
      false);

  // `FS.chown(path, owner=nil, group=nil)` — change ownership. `owner`/`group`
  // each accept a name (String), a numeric id (Long), or nil (leave that one
  // unchanged). IOError on a missing path, unknown name, or permission failure
  // (changing the owner usually requires root).
  ns.initialize(
      "chown",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv},
           {"owner", false, ""sv, nullptr, kw_default_nil()},
           {"group", false, ""sv, nullptr, kw_default_nil()}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& p = _fspath(env->get("path"));
            auto resolve = [&](const Value& v, const char* param,
                               bool is_user) -> int64_t {
              if (v.type == Value::Nil) return -1;
              if (v.type == Value::Long) return v.get<int64_t>();
              if (v.is_stringlike()) {  // String or StringView (e.g. a slice)
                const auto name = v.to_string();
                int64_t id = is_user ? _fs_uid_from_name(name)
                                  : _fs_gid_from_name(name);
                if (id < 0) {
                  throw CulebraError(
                      "IOError",
                      std::format("FS.chown: unknown {} '{}'",
                                  is_user ? "user" : "group", name),
                      line, col);
                }
                return id;
              }
              throw CulebraError(
                  "TypeError",
                  std::format("type error: parameter '{}' expects "
                              "String, Long, or Nil",
                              param),
                  line, col);
            };
            int64_t uid = resolve(env->get("owner"), "owner", true);
            int64_t gid = resolve(env->get("group"), "group", false);
            _fs_do_chown(std::filesystem::path(p), uid, gid, line, col);
            return Value();
          })),
      false);

  // `FS.rename(src, dst)` — atomic within a filesystem. IOError otherwise.
  ns.initialize(
      "rename",
      Value(FunctionValue(
          {{"src", false, "String|Path"sv}, {"dst", false, "String|Path"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& s = _fspath(env->get("src"));
            const auto& d = _fspath(env->get("dst"));
            std::error_code ec;
            std::filesystem::rename(s, d, ec);
            if (ec) throw_io(std::format("FS.rename('{}', '{}')", s, d),
                             line, col, ec);
            return Value();
          })),
      false);

  // `FS.copy(src, dst, recursive: false)` — file or (recursive) tree copy.
  ns.initialize(
      "copy",
      Value(FunctionValue(
          {{"src", false, "String|Path"sv}, {"dst", false, "String|Path"sv},
           {"recursive", false, ""sv, nullptr, kw_default_false()}},
          [throw_io](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& s = _fspath(env->get("src"));
            const auto& d = _fspath(env->get("dst"));
            bool recursive = env->get("recursive").to_bool();
            std::error_code ec;
            auto opts = std::filesystem::copy_options::overwrite_existing;
            if (recursive) opts |= std::filesystem::copy_options::recursive;
            std::filesystem::copy(s, d, opts, ec);
            if (ec) throw_io(std::format("FS.copy('{}', '{}')", s, d),
                             line, col, ec);
            return Value();
          })),
      false);

  // --- Path resolution ---
  ns.initialize("normpath",
                string_to_string([](const std::filesystem::path& p) {
                  return p.lexically_normal().string();
                }),
                false);
  ns.initialize("is_abs",
                path_query_bool([](const std::filesystem::path& p,
                                   std::error_code&) {
                  return p.is_absolute();
                }),
                false);
  ns.initialize(
      "abspath",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& p = _fspath(env->get("path"));
            std::error_code ec;
            auto out = std::filesystem::absolute(p, ec);
            if (ec) throw_io(std::format("FS.abspath('{}')", p), line, col, ec);
            return Value(out.lexically_normal().string());
          },
          "String"sv)),
      false);
  ns.initialize(
      "realpath",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& p = _fspath(env->get("path"));
            std::error_code ec;
            auto out = std::filesystem::weakly_canonical(p, ec);
            if (ec) throw_io(std::format("FS.realpath('{}')", p), line, col, ec);
            return Value(out.string());
          },
          "String"sv)),
      false);

  // --- Symlinks ---
  ns.initialize("is_symlink",
                path_query_bool([](const std::filesystem::path& p,
                                   std::error_code& ec) {
                  return std::filesystem::is_symlink(
                      std::filesystem::symlink_status(p, ec));
                }),
                false);
  ns.initialize(
      "symlink",
      Value(FunctionValue(
          {{"target", false, "String|Path"sv}, {"link", false, "String|Path"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& t = _fspath(env->get("target"));
            const auto& l = _fspath(env->get("link"));
            std::error_code ec;
            std::filesystem::create_symlink(t, l, ec);
            if (ec) throw_io(std::format("FS.symlink('{}', '{}')", t, l),
                             line, col, ec);
            return Value();
          })),
      false);
  ns.initialize(
      "readlink",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& p = _fspath(env->get("path"));
            std::error_code ec;
            auto out = std::filesystem::read_symlink(p, ec);
            if (ec) throw_io(std::format("FS.readlink('{}')", p), line, col, ec);
            return Value(out.string());
          },
          "String"sv)),
      false);

  // `FS.walk(path)` -> Array<String> of all paths under `path`, recursive,
  // depth-first. Each entry is the full path (root-relative as given).
  ns.initialize(
      "walk",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv}},
          [throw_io](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& p = _fspath(env->get("path"));
            std::error_code ec;
            std::filesystem::recursive_directory_iterator it(p, ec);
            if (ec) throw_io(std::format("FS.walk('{}')", p), line, col, ec);
            ArrayValue av;
            std::error_code iter_ec;
            for (auto end = std::filesystem::recursive_directory_iterator();
                 it != end; it.increment(iter_ec)) {
              if (iter_ec) break;
              av.values->emplace_back(it->path().string());
            }
            return Value(std::move(av));
          },
          "Array"sv)),
      false);

  // `FS.glob(pattern)` -> Array<String> of matching paths. Supports `*`,
  // `?`, `[...]` per path segment, and `**` for recursive descent.
  ns.initialize(
      "glob",
      Value(FunctionValue(
          {{"pattern", false, "String"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            const auto& pat = env->get("pattern").to_string();
            ArrayValue av;
            for (auto& m : _fs_glob(pat)) av.values->emplace_back(std::move(m));
            return Value(std::move(av));
          },
          "Array"sv)),
      false);

  return Value(std::move(ns));
}

// --- File handle side table (file-scope, shared between interp + JIT) ---
//
// A File handle is an Object carrying just `_id` (Long); the native
// std::fstream lives here, keyed by id. The table is per-Runtime (Runtime
// is thread_local), so handles don't cross isolate/copy boundaries — which
// matches the concurrency model. close()/drop erase the entry (idempotent:
// a missing id is a no-op or "closed file" error depending on the op).

struct _FileStream {
  std::fstream fs;
  char mode;        // 'r' / 'w' / 'a'
  bool readable;
  bool writable;
};

struct _FileTable {
  std::unordered_map<int64_t, _FileStream> entries;
  int64_t next_id = 1;
};

inline _FileTable& _file_table() {
  return runtime_substate<_FileTable>(kSlotFileTable);
}

[[noreturn]] inline void _file_throw(const std::string& what, int64_t line,
                                     int64_t col, std::string_view kind = "IOError") {
  throw CulebraError(std::string(kind), what, line, col);
}

// Open `path` in `mode` (r/w/a). Returns a fresh handle id. ValueError on
// bad mode, IOError if the stream can't be opened.
inline int64_t _file_open(const std::string& path, const std::string& mode,
                          int64_t line, int64_t col) {
  std::ios::openmode flags = std::ios::binary;
  bool readable = false, writable = false;
  if (mode == "r")      { flags |= std::ios::in;  readable = true; }
  else if (mode == "w") { flags |= std::ios::out | std::ios::trunc; writable = true; }
  else if (mode == "a") { flags |= std::ios::out | std::ios::app;   writable = true; }
  else {
    _file_throw(std::format("File.open: invalid mode '{}' (expected r/w/a)",
                            mode), line, col, "ValueError");
  }
  auto& tbl = _file_table();
  int64_t id = tbl.next_id++;
  auto& slot = tbl.entries[id];
  slot.fs.open(path, flags);
  slot.mode = mode[0];
  slot.readable = readable;
  slot.writable = writable;
  if (!slot.fs.is_open()) {
    tbl.entries.erase(id);
    _file_throw(std::format("File.open('{}', '{}')", path, mode), line, col);
  }
  return id;
}

// Look up a live stream; IOError if the id was closed.
inline _FileStream& _file_get(int64_t id, const char* op, int64_t line, int64_t col) {
  auto& tbl = _file_table();
  auto it = tbl.entries.find(id);
  if (it == tbl.entries.end()) {
    _file_throw(std::format("File.{}: operation on closed file", op), line, col);
  }
  return it->second;
}

inline std::string _file_read_all(int64_t id, int64_t line, int64_t col) {
  auto& s = _file_get(id, "read", line, col);
  if (!s.readable) _file_throw("File.read: file not opened for reading", line, col);
  std::string out((std::istreambuf_iterator<char>(s.fs)),
                  std::istreambuf_iterator<char>());
  return out;
}

inline std::string _file_read_n(int64_t id, int64_t n, int64_t line, int64_t col) {
  auto& s = _file_get(id, "read", line, col);
  if (!s.readable) _file_throw("File.read: file not opened for reading", line, col);
  if (n < 0) n = 0;
  std::string buf(static_cast<size_t>(n), '\0');
  s.fs.read(buf.data(), n);
  buf.resize(static_cast<size_t>(s.fs.gcount()));
  return buf;
}

inline void _file_write(int64_t id, std::string_view data, int64_t line, int64_t col) {
  auto& s = _file_get(id, "write", line, col);
  if (!s.writable) _file_throw("File.write: file not opened for writing", line, col);
  s.fs.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (s.fs.bad()) _file_throw("File.write: write failed", line, col);
}

inline void _file_flush(int64_t id, int64_t line, int64_t col) {
  _file_get(id, "flush", line, col).fs.flush();
}

inline void _file_seek(int64_t id, int64_t off, std::string_view whence,
                       int64_t line, int64_t col) {
  auto& s = _file_get(id, "seek", line, col);
  std::ios::seekdir dir;
  if (whence == "set")      dir = std::ios::beg;
  else if (whence == "cur") dir = std::ios::cur;
  else if (whence == "end") dir = std::ios::end;
  else _file_throw(std::format("File.seek: invalid whence '{}' "
                               "(expected set/cur/end)", whence),
                   line, col, "ValueError");
  s.fs.clear();  // clear EOF so seeking past a prior read works
  if (s.readable) s.fs.seekg(off, dir); else s.fs.seekp(off, dir);
}

inline int64_t _file_tell(int64_t id, int64_t line, int64_t col) {
  auto& s = _file_get(id, "tell", line, col);
  return static_cast<int64_t>(s.readable ? s.fs.tellg() : s.fs.tellp());
}

// Read one line (newline stripped, handles \n / \r\n / \r). Returns false
// at end of stream.
inline bool _file_getline(int64_t id, std::string& out, int64_t line, int64_t col) {
  auto& s = _file_get(id, "lines", line, col);
  out.clear();
  int ch;
  bool any = false;
  while ((ch = s.fs.get()) != EOF) {
    any = true;
    if (ch == '\n') return true;
    if (ch == '\r') {
      if (s.fs.peek() == '\n') s.fs.get();
      return true;
    }
    out.push_back(static_cast<char>(ch));
  }
  return any;
}

inline void _file_close(int64_t id) {
  _file_table().entries.erase(id);  // idempotent: missing id is a no-op
}

// Attach a `dispose` method to a lines()/chunks() iterator: closes the
// handle on for-in exit (incl. break). `fn` is captured only to keep the
// handle Value alive until the loop drains (otherwise the anonymous
// `File.open(p).lines()` handle would be GC'd before iteration).
inline void _file_attach_dispose(Value& iter, Value self, int64_t id) {
  iter.to_object().initialize(
      "dispose",
      Value(FunctionValue({}, [self, id](std::shared_ptr<Environment>) {
        _file_close(id);
        return Value();
      })),
      false);
}

// --- Time helpers (file-scope, shared between interp + future JIT path) ---

namespace _time_detail {

// Days-in-month with leap year support.
inline int days_in_month(int year, int month) {
  static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) {
    bool leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
    return leap ? 29 : 28;
  }
  return days[month - 1];
}

[[noreturn]] inline void throw_value(const std::string& msg, int64_t line, int64_t col) {
  throw CulebraError("ValueError", msg, line, col);
}

inline int64_t iso_weekday(const std::tm& tm) {
  // tm_wday is 0=Sun..6=Sat; ISO 8601 uses 0=Mon..6=Sun.
  return (tm.tm_wday + 6) % 7;
}

// --- Long-nanos helpers ------------------------------------------------
//
// i64 nanoseconds since Unix epoch, covers ±292 years from 1970 — ample
// for any practical use, and preserves full nanosecond precision (Float
// Unix seconds only get ~400ns near current epoch).

inline constexpr int64_t NS_PER_SEC = 1'000'000'000;

// Floor-divide nanos into (whole_seconds, sub_seconds_nanos in [0, 1e9)).
// Truncating `%` in C++ misbehaves for negative `nanos`, hence the fixup.
inline std::pair<std::time_t, int64_t> split_nanos(int64_t nanos) {
  auto secs = nanos / NS_PER_SEC;
  auto sub  = nanos % NS_PER_SEC;
  if (sub < 0) { secs -= 1; sub += NS_PER_SEC; }
  return {static_cast<std::time_t>(secs), sub};
}

inline int64_t combine_nanos(std::time_t secs, int64_t sub_nanos) {
  return static_cast<int64_t>(secs) * NS_PER_SEC + sub_nanos;
}

inline std::tm to_tm_nanos(int64_t nanos, bool utc) {
  auto t = split_nanos(nanos).first;
  std::tm tm{};
  if (utc) os_gmtime_r(&t, &tm); else os_localtime_r(&t, &tm);
  return tm;
}

inline int64_t from_tm_nanos(std::tm& tm, int64_t sub_nanos, bool utc) {
  tm.tm_isdst = -1;
  auto t = utc ? os_timegm(&tm) : std::mktime(&tm);
  return combine_nanos(t, sub_nanos);
}

inline std::optional<int64_t> parse_iso_nanos(std::string_view s) {
  // Mirror parse_iso() but accumulate sub-second as i64 nanos with up to
  // 9 digits of precision (trailing digits past 9 are discarded).
  if (s.size() < 10) return std::nullopt;
  std::tm tm{};
  auto parse_int = [&](size_t off, int n, int& out) -> bool {
    if (off + n > s.size()) return false;
    int v = 0;
    for (int i = 0; i < n; i++) {
      auto c = s[off + i];
      if (c < '0' || c > '9') return false;
      v = v * 10 + (c - '0');
    }
    out = v;
    return true;
  };
  int y, mo, d, h = 0, mi = 0, se = 0;
  if (!parse_int(0, 4, y) || s[4] != '-' || !parse_int(5, 2, mo) ||
      s[7] != '-' || !parse_int(8, 2, d)) {
    return std::nullopt;
  }
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  int64_t sub_ns = 0;
  long offset_seconds = 0;
  bool has_tz = false;
  size_t i = 10;
  if (i < s.size() && (s[i] == 'T' || s[i] == ' ')) {
    i++;
    if (!parse_int(i, 2, h) || (i + 2 < s.size() && s[i + 2] != ':')) {
      return std::nullopt;
    }
    i += 3;
    if (!parse_int(i, 2, mi)) return std::nullopt;
    i += 2;
    if (i < s.size() && s[i] == ':') {
      i++;
      if (!parse_int(i, 2, se)) return std::nullopt;
      i += 2;
    }
    if (i < s.size() && s[i] == '.') {
      i++;
      int digit_count = 0;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (digit_count < 9) {
          sub_ns = sub_ns * 10 + (s[i] - '0');
          digit_count++;
        }
        i++;
      }
      while (digit_count < 9) { sub_ns *= 10; digit_count++; }
    }
    if (i < s.size()) {
      auto c = s[i];
      if (c == 'Z') { has_tz = true; i++; }
      else if (c == '+' || c == '-') {
        int sign = (c == '+') ? 1 : -1;
        i++;
        int oh, om = 0;
        if (!parse_int(i, 2, oh)) return std::nullopt;
        i += 2;
        if (i < s.size() && s[i] == ':') i++;
        if (i + 2 <= s.size() && s[i] >= '0' && s[i] <= '9') {
          if (!parse_int(i, 2, om)) return std::nullopt;
          i += 2;
        }
        offset_seconds = sign * (oh * 3600 + om * 60);
        has_tz = true;
      }
    }
  }
  if (i != s.size()) return std::nullopt;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = se;
  tm.tm_isdst = 0;
  // Date-only / tz-less → treat as UTC (deterministic across hosts).
  std::time_t t = os_timegm(&tm);
  if (has_tz) t -= offset_seconds;
  return combine_nanos(t, sub_ns);
}

inline std::string format_iso_nanos(int64_t nanos, bool utc) {
  auto [t, sub] = split_nanos(nanos);
  std::tm tm{};
  if (utc) os_gmtime_r(&t, &tm); else os_localtime_r(&t, &tm);
  std::string tz_str = "Z";
  if (!utc) {
    auto offset = os_gmtoff(tm, t);
    int sign = offset < 0 ? -1 : 1;
    int64_t abs_off = std::abs(static_cast<long>(offset));
    tz_str = std::format("{}{:02d}:{:02d}",
                         sign < 0 ? '-' : '+',
                         static_cast<int>(abs_off / 3600),
                         static_cast<int>((abs_off % 3600) / 60));
  }
  if (sub == 0) {
    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}{}",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec, tz_str);
  }
  return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:09d}{}",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec,
                     static_cast<int>(sub), tz_str);
}

inline std::string format_strftime_nanos(int64_t nanos,
                                         const std::string& fmt, bool utc) {
  auto t = split_nanos(nanos).first;
  std::tm tm{};
  if (utc) os_gmtime_r(&t, &tm); else os_localtime_r(&t, &tm);
  char buf[256];
  auto n = std::strftime(buf, sizeof(buf), fmt.c_str(), &tm);
  return std::string(buf, n);
}

}  // namespace _time_detail

// `_Time`: thin Long-nanos primitives. The user-facing `Time` module
// (Instant / Duration classes — `TIME_MODULE_SOURCE` below) wraps
// these to deliver natural calendar arithmetic + operator overloads.
// Underscore prefix marks it as the wrapper's ABI, not a stable
// surface for direct use.
//
// All functions are positional-only so the JIT path stays simple.
inline Value make_time_primitives_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  // _Time.now_nanos() -> Long (Unix epoch nanos)
  ns.initialize("now_nanos",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            using clock = std::chrono::system_clock;
            auto d = clock::now().time_since_epoch();
            auto ns_count = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
            return Value(static_cast<int64_t>(ns_count));
          },
          "Long"sv)),
      false);

  // _Time.monotonic() -> Float (seconds since first call / process start)
  ns.initialize("monotonic",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            using clock = std::chrono::steady_clock;
            static const auto t0 = clock::now();
            auto d = clock::now() - t0;
            return Value(std::chrono::duration<double>(d).count());
          },
          "Float"sv)),
      false);

  // _Time.sleep(secs: Float) -> Nil
  ns.initialize("sleep",
      Value(FunctionValue({{"secs", false, "Float"sv}},
          [](std::shared_ptr<Environment> env) {
            auto secs = env->get("secs").to_double_coerce();
            if (secs > 0) {
              std::this_thread::sleep_for(std::chrono::duration<double>(secs));
            }
            return Value();
          })),
      false);

  // _Time.from_iso_nanos(s: String) -> Long
  ns.initialize("from_iso_nanos",
      Value(FunctionValue({{"s", false, "String"sv}},
          [](std::shared_ptr<Environment> env) {
            const auto& s = env->get("s").to_string();
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            auto r = _time_detail::parse_iso_nanos(s);
            if (!r) _time_detail::throw_value(
                std::format("_Time.from_iso_nanos: invalid ISO 8601 '{}'", s),
                line, col);
            return Value(static_cast<int64_t>(*r));
          },
          "Long"sv)),
      false);

  // _Time.parse_nanos(s, fmt) -> Long (strftime)
  ns.initialize("parse_nanos",
      Value(FunctionValue(
          {{"s", false, "String"sv}, {"fmt", false, "String"sv}},
          [](std::shared_ptr<Environment> env) {
            const auto& s = env->get("s").to_string();
            const auto& fmt = env->get("fmt").to_string();
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            std::tm tm{};
            if (!os_strptime(s.c_str(), fmt.c_str(), &tm)) {
              _time_detail::throw_value(
                  std::format("_Time.parse_nanos: '{}' does not match '{}'", s, fmt),
                  line, col);
            }
            tm.tm_isdst = -1;
            auto t = std::mktime(&tm);
            return Value(static_cast<int64_t>(_time_detail::combine_nanos(t, 0)));
          },
          "Long"sv)),
      false);

  // _Time.iso_nanos(nanos: Long, utc: Bool) -> String
  ns.initialize("iso_nanos",
      Value(FunctionValue(
          {{"nanos", false, "Long"sv}, {"utc", false, "Bool"sv}},
          [](std::shared_ptr<Environment> env) {
            auto nanos = env->get("nanos").to_long();
            auto utc = env->get("utc").to_bool();
            return Value(_time_detail::format_iso_nanos(nanos, utc));
          },
          "String"sv)),
      false);

  // _Time.format_nanos(nanos, fmt, utc) -> String
  ns.initialize("format_nanos",
      Value(FunctionValue(
          {{"nanos", false, "Long"sv},
           {"fmt", false, "String"sv},
           {"utc", false, "Bool"sv}},
          [](std::shared_ptr<Environment> env) {
            auto nanos = env->get("nanos").to_long();
            const auto& fmt = env->get("fmt").to_string();
            auto utc = env->get("utc").to_bool();
            return Value(_time_detail::format_strftime_nanos(nanos, fmt, utc));
          },
          "String"sv)),
      false);

  // _Time.parts_nanos(nanos, utc) -> Object
  ns.initialize("parts_nanos",
      Value(FunctionValue(
          {{"nanos", false, "Long"sv}, {"utc", false, "Bool"sv}},
          [](std::shared_ptr<Environment> env) {
            auto nanos = env->get("nanos").to_long();
            auto utc = env->get("utc").to_bool();
            auto tm = _time_detail::to_tm_nanos(nanos, utc);
            auto sub = _time_detail::split_nanos(nanos).second;
            ObjectValue p;
            p.initialize("year",      Value(static_cast<int64_t>(tm.tm_year + 1900)), false);
            p.initialize("month",     Value(static_cast<int64_t>(tm.tm_mon + 1)),     false);
            p.initialize("day",       Value(static_cast<int64_t>(tm.tm_mday)),        false);
            p.initialize("hour",      Value(static_cast<int64_t>(tm.tm_hour)),        false);
            p.initialize("minute",    Value(static_cast<int64_t>(tm.tm_min)),         false);
            p.initialize("second",    Value(static_cast<int64_t>(tm.tm_sec)),         false);
            p.initialize("nanosecond",Value(static_cast<int64_t>(sub)),               false);
            p.initialize("weekday",   Value(_time_detail::iso_weekday(tm)),        false);
            p.initialize("dayofyear", Value(static_cast<int64_t>(tm.tm_yday + 1)),    false);
            return Value(std::move(p));
          },
          "Object"sv)),
      false);

  // _Time.from_parts_nanos(p: Object, utc: Bool) -> Long
  ns.initialize("from_parts_nanos",
      Value(FunctionValue(
          {{"p", false, "Object"sv}, {"utc", false, "Bool"sv}},
          [](std::shared_ptr<Environment> env) {
            const auto& obj = env->get("p").to_object();
            auto utc = env->get("utc").to_bool();
            auto get = [&](const char* k, int64_t fallback) -> int64_t {
              if (obj.has(k)) return obj.get(k).to_long();
              return fallback;
            };
            std::tm tm{};
            tm.tm_year = static_cast<int>(get("year", 1970) - 1900);
            tm.tm_mon  = static_cast<int>(get("month", 1) - 1);
            tm.tm_mday = static_cast<int>(get("day", 1));
            tm.tm_hour = static_cast<int>(get("hour", 0));
            tm.tm_min  = static_cast<int>(get("minute", 0));
            tm.tm_sec  = static_cast<int>(get("second", 0));
            auto sub_ns = get("nanosecond", 0);
            return Value(static_cast<int64_t>(_time_detail::from_tm_nanos(tm, sub_ns, utc)));
          },
          "Long"sv)),
      false);

  // _Time.weekday_nanos(nanos, utc) -> Long (0=Mon..6=Sun)
  ns.initialize("weekday_nanos",
      Value(FunctionValue(
          {{"nanos", false, "Long"sv}, {"utc", false, "Bool"sv}},
          [](std::shared_ptr<Environment> env) {
            auto nanos = env->get("nanos").to_long();
            auto utc = env->get("utc").to_bool();
            return Value(_time_detail::iso_weekday(_time_detail::to_tm_nanos(nanos, utc)));
          },
          "Long"sv)),
      false);

  // _Time.add_nanos(nanos, years, months, days, hours, minutes, seconds, utc) -> Long
  ns.initialize("add_nanos",
      Value(FunctionValue(
          {{"nanos",   false, "Long"sv},
           {"years",   false, "Long"sv},
           {"months",  false, "Long"sv},
           {"days",    false, "Long"sv},
           {"hours",   false, "Long"sv},
           {"minutes", false, "Long"sv},
           {"seconds", false, "Long"sv},
           {"utc",     false, "Bool"sv}},
          [](std::shared_ptr<Environment> env) {
            auto nanos = env->get("nanos").to_long();
            auto utc = env->get("utc").to_bool();
            auto tm = _time_detail::to_tm_nanos(nanos, utc);
            auto sub = _time_detail::split_nanos(nanos).second;
            int64_t years_add  = env->get("years").to_long();
            int64_t months_add = env->get("months").to_long();
            if (years_add || months_add) {
              int target_year = tm.tm_year + 1900 + static_cast<int>(years_add);
              int target_month_total = tm.tm_mon + static_cast<int>(months_add);
              target_year += target_month_total / 12;
              int target_month = target_month_total % 12;
              if (target_month < 0) { target_month += 12; target_year -= 1; }
              int last = _time_detail::days_in_month(target_year, target_month + 1);
              int target_day = tm.tm_mday > last ? last : tm.tm_mday;
              tm.tm_year = target_year - 1900;
              tm.tm_mon = target_month;
              tm.tm_mday = target_day;
            }
            tm.tm_mday += static_cast<int>(env->get("days").to_long());
            tm.tm_hour += static_cast<int>(env->get("hours").to_long());
            tm.tm_min  += static_cast<int>(env->get("minutes").to_long());
            tm.tm_sec  += static_cast<int>(env->get("seconds").to_long());
            return Value(static_cast<int64_t>(_time_detail::from_tm_nanos(tm, sub, utc)));
          },
          "Long"sv)),
      false);

  // _Time.start_of_nanos(nanos, unit, utc) -> Long
  ns.initialize("start_of_nanos",
      Value(FunctionValue(
          {{"nanos", false, "Long"sv},
           {"unit",  false, "String"sv},
           {"utc",   false, "Bool"sv}},
          [](std::shared_ptr<Environment> env) {
            auto nanos = env->get("nanos").to_long();
            const auto& unit = env->get("unit").to_string();
            auto utc = env->get("utc").to_bool();
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            auto tm = _time_detail::to_tm_nanos(nanos, utc);
            if (unit == "year")        { tm.tm_mon = 0; tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; }
            else if (unit == "month")  { tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; }
            else if (unit == "day")    { tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; }
            else if (unit == "hour")   { tm.tm_min = 0; tm.tm_sec = 0; }
            else if (unit == "minute") { tm.tm_sec = 0; }
            else {
              _time_detail::throw_value(
                  std::format("_Time.start_of_nanos: unknown unit '{}' "
                              "(year/month/day/hour/minute)", unit),
                  line, col);
            }
            return Value(static_cast<int64_t>(_time_detail::from_tm_nanos(tm, 0, utc)));
          },
          "Long"sv)),
      false);

  return Value(std::move(ns));
}

// `_Term`: thin terminal-control primitives (raw mode / size / timed key
// read). The user-facing `Term` module (colours, `Screen`, `Key`, the
// `app` render-loop wrapper — `TERM_MODULE_SOURCE` below) is culebra source
// layered on top. Underscore-prefixed: the wrapper's ABI, not stable API.
// All positional-only so the JIT fast path stays simple.
inline Value make_term_primitives_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  // _Term.cols() / _Term.rows() -> Long (terminal cells; 80x24 off a tty)
  ns.initialize("cols",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(static_cast<int64_t>(_term_detail::cols()));
          },
          "Long"sv)),
      false);
  ns.initialize("rows",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(static_cast<int64_t>(_term_detail::rows()));
          },
          "Long"sv)),
      false);

  // _Term.raw_on() / _Term.raw_off() -> Nil (termios; no-op off a tty)
  ns.initialize("raw_on",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            _term_detail::raw_on();
            return Value();
          })),
      false);
  ns.initialize("raw_off",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            _term_detail::raw_off();
            return Value();
          })),
      false);

  // _Term.flush() -> Nil (flush buffered stdout — surface a built frame)
  ns.initialize("flush",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            _term_detail::flush();
            return Value();
          })),
      false);

  // _Term.color_level() -> Long (0 none / 1 16 / 2 256 / 3 truecolor)
  ns.initialize("color_level",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(static_cast<int64_t>(_term_detail::color_level()));
          },
          "Long"sv)),
      false);

  // _Term.width(s: StringLike) -> Long (display columns; wide/emoji = 2)
  ns.initialize("width",
      Value(FunctionValue({{"s", false, "StringLike"sv}},
          [](std::shared_ptr<Environment> env) {
            return Value(static_cast<int64_t>(_term_detail::width(
                std::string(env->get("s").to_string_view()))));
          },
          "Long"sv)),
      false);

  // _Term.resized() -> Bool (true once after a SIGWINCH terminal resize)
  ns.initialize("resized",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(_term_detail::take_resize());
          },
          "Bool"sv)),
      false);

  // _Term.read_key(timeout: Float) -> String (raw bytes; "" on timeout)
  ns.initialize("read_key",
      Value(FunctionValue({{"timeout", false, "Float"sv}},
          [](std::shared_ptr<Environment> env) {
            auto t = env->get("timeout").to_double_coerce();
            return Value(_term_detail::read_key(t));
          },
          "String"sv)),
      false);

  // _Term.attach_tty() -> Bool (reattach stdin to the controlling terminal;
  // false if none exists)
  ns.initialize("attach_tty",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(_term_detail::attach_tty());
          },
          "Bool"sv)),
      false);

  return Value(std::move(ns));
}

// `_Canvas`: thin immediate-mode 2D framebuffer primitives (pixel buffer,
// sprite blit, present, polled input, tone). The user-facing `Canvas` module
// (colours, `Sprite`, the bitmap font, the `run` loop — `CANVAS_MODULE_SOURCE`
// below) is culebra source layered on top. Underscore-prefixed: the wrapper's
// ABI, not stable API. All positional-only so the JIT fast path stays simple.
//
// Geometry parameters are annotated `Long|Float` and read through `coord_arg`,
// so a caller that computes positions in floating point (a projection, a
// scroll offset) hands them over directly instead of paying a `Math.floor` per
// argument. Colours, flags and handles stay `Long`.
// A `_Canvas` geometry argument as a pixel index: a Long is already one, a
// Float goes through the single shared rounding rule in canvas.h. The param is
// annotated `Long|Float`, so nothing else reaches here.
inline int64_t canvas_coord_arg(const std::shared_ptr<Environment>& env,
                                const char* name) {
  const auto& v = env->get(name);
  return v.type == Value::Long ? static_cast<int64_t>(v.to_long())
                               : _canvas_detail::coord(v.get<double>());
}

// A `_Canvas` Array parameter as a packed integer vector. The param is
// annotated `Array`, so only the per-element narrowing happens here.
template <typename T>
inline std::vector<T> canvas_int_array_arg(
    const std::shared_ptr<Environment>& env, const char* name) {
  const auto& arr = *env->get(name).to_array().values;
  std::vector<T> out;
  out.reserve(arr.size());
  for (const auto& v : arr) out.push_back(static_cast<T>(v.to_long()));
  return out;
}

inline Value make_canvas_primitives_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  // _Canvas.init(w, h) -> Nil (allocate/reset the framebuffer). Raises when
  // another isolate owns the canvas — the one drawing call that says so.
  ns.initialize("init",
      Value(FunctionValue({{"w", false, "Long"sv}, {"h", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            if (!_canvas_detail::init(env->get("w").to_long(),
                                      env->get("h").to_long()))
              throw CulebraError("RuntimeError", _canvas_detail::kBusyError,
                                 env->get("__LINE__").to_long(),
                                 env->get("__COLUMN__").to_long());
            return Value();
          })),
      false);

  // _Canvas.clear(rgba) -> Nil (fill the whole framebuffer)
  ns.initialize("clear",
      Value(FunctionValue({{"rgba", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::clear(
                static_cast<uint32_t>(env->get("rgba").to_long()));
            return Value();
          })),
      false);

  // _Canvas.set_pixel(x, y, rgba) -> Nil
  ns.initialize("set_pixel",
      Value(FunctionValue({{"x", false, "Long|Float"sv},
                           {"y", false, "Long|Float"sv},
                           {"rgba", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::set_pixel(
                canvas_coord_arg(env, "x"), canvas_coord_arg(env, "y"),
                static_cast<uint32_t>(env->get("rgba").to_long()));
            return Value();
          })),
      false);

  // _Canvas.get_pixel(x, y) -> Long (packed RGBA; 0 off-buffer)
  ns.initialize("get_pixel",
      Value(FunctionValue({{"x", false, "Long|Float"sv},
                           {"y", false, "Long|Float"sv}},
          [](std::shared_ptr<Environment> env) {
            return Value(static_cast<int64_t>(_canvas_detail::get_pixel(
                canvas_coord_arg(env, "x"), canvas_coord_arg(env, "y"))));
          },
          "Long"sv)),
      false);

  // _Canvas.rect(x, y, w, h, rgba, fill) -> Nil (clipped; fill 0 = outline)
  ns.initialize("rect",
      Value(FunctionValue({{"x", false, "Long|Float"sv},
                           {"y", false, "Long|Float"sv},
                           {"w", false, "Long|Float"sv},
                           {"h", false, "Long|Float"sv},
                           {"rgba", false, "Long"sv},
                           {"fill", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::rect(
                canvas_coord_arg(env, "x"), canvas_coord_arg(env, "y"),
                canvas_coord_arg(env, "w"), canvas_coord_arg(env, "h"),
                static_cast<uint32_t>(env->get("rgba").to_long()),
                env->get("fill").to_long() != 0);
            return Value();
          })),
      false);

  // _Canvas.line(x1, y1, x2, y2, rgba) -> Nil (both endpoints included)
  ns.initialize("line",
      Value(FunctionValue({{"x1", false, "Long|Float"sv},
                           {"y1", false, "Long|Float"sv},
                           {"x2", false, "Long|Float"sv},
                           {"y2", false, "Long|Float"sv},
                           {"rgba", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::line(
                canvas_coord_arg(env, "x1"), canvas_coord_arg(env, "y1"),
                canvas_coord_arg(env, "x2"), canvas_coord_arg(env, "y2"),
                static_cast<uint32_t>(env->get("rgba").to_long()));
            return Value();
          })),
      false);

  // _Canvas.ellipse(cx, cy, rx, ry, rgba, fill) -> Nil (clipped; circle when
  // rx == ry)
  ns.initialize("ellipse",
      Value(FunctionValue({{"cx", false, "Long|Float"sv},
                           {"cy", false, "Long|Float"sv},
                           {"rx", false, "Long|Float"sv},
                           {"ry", false, "Long|Float"sv},
                           {"rgba", false, "Long"sv},
                           {"fill", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::ellipse(
                canvas_coord_arg(env, "cx"), canvas_coord_arg(env, "cy"),
                canvas_coord_arg(env, "rx"), canvas_coord_arg(env, "ry"),
                static_cast<uint32_t>(env->get("rgba").to_long()),
                env->get("fill").to_long() != 0);
            return Value();
          })),
      false);

  // _Canvas.triangle(x1, y1, x2, y2, x3, y3, rgba, fill) -> Nil (clipped)
  ns.initialize("triangle",
      Value(FunctionValue({{"x1", false, "Long|Float"sv},
                           {"y1", false, "Long|Float"sv},
                           {"x2", false, "Long|Float"sv},
                           {"y2", false, "Long|Float"sv},
                           {"x3", false, "Long|Float"sv},
                           {"y3", false, "Long|Float"sv},
                           {"rgba", false, "Long"sv},
                           {"fill", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::triangle(
                canvas_coord_arg(env, "x1"), canvas_coord_arg(env, "y1"),
                canvas_coord_arg(env, "x2"), canvas_coord_arg(env, "y2"),
                canvas_coord_arg(env, "x3"), canvas_coord_arg(env, "y3"),
                static_cast<uint32_t>(env->get("rgba").to_long()),
                env->get("fill").to_long() != 0);
            return Value();
          })),
      false);

  // _Canvas.polygon(points: Array, rgba, fill) -> Nil (clipped, even-odd).
  // points is a flat x0, y0, x1, y1, ... of Long|Float; the outline closes
  // automatically.
  ns.initialize("polygon",
      Value(FunctionValue({{"points", false, "Array"sv},
                           {"rgba", false, "Long"sv},
                           {"fill", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& arr = *env->get("points").to_array().values;
            std::vector<int64_t> pts;
            pts.reserve(arr.size());
            for (const auto& v : arr) {
              if (v.type == Value::Long) {
                pts.push_back(static_cast<int64_t>(v.to_long()));
              } else if (v.type == Value::Float) {
                pts.push_back(_canvas_detail::coord(v.get<double>()));
              } else {
                throw CulebraError("TypeError",
                                   _canvas_detail::kPolygonPointsError, line,
                                   col);
              }
            }
            _canvas_detail::polygon(
                pts.data(), static_cast<int64_t>(pts.size() / 2),
                static_cast<uint32_t>(env->get("rgba").to_long()),
                env->get("fill").to_long() != 0);
            return Value();
          })),
      false);

  // _Canvas.font_load(rows: Array) -> Long (handle). rows is a flat array of
  // 8 row-bytes per glyph; the table is uploaded once and glyph() re-references
  // it, the same upload-once shape as sprite_load.
  ns.initialize("font_load",
      Value(FunctionValue({{"rows", false, "Array"sv}},
          [](std::shared_ptr<Environment> env) {
            auto rows = canvas_int_array_arg<uint8_t>(env, "rows");
            return Value(static_cast<int64_t>(_canvas_detail::font_load(
                rows.data(), static_cast<int64_t>(rows.size()))));
          },
          "Long"sv)),
      false);

  // _Canvas.glyph(font, index, x, y, rgba, scale) -> Nil (one 8x8 glyph,
  // clipped, each font pixel a scale x scale block)
  ns.initialize("glyph",
      Value(FunctionValue({{"font", false, "Long"sv},
                           {"index", false, "Long"sv},
                           {"x", false, "Long|Float"sv},
                           {"y", false, "Long|Float"sv},
                           {"rgba", false, "Long"sv},
                           {"scale", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::glyph(
                env->get("font").to_long(), env->get("index").to_long(),
                canvas_coord_arg(env, "x"), canvas_coord_arg(env, "y"),
                static_cast<uint32_t>(env->get("rgba").to_long()),
                env->get("scale").to_long());
            return Value();
          })),
      false);

  // _Canvas.sprite_load(pixels: Array, w, h) -> Long (handle). pixels is a
  // flat row-major array of packed-RGBA Longs.
  ns.initialize("sprite_load",
      Value(FunctionValue({{"pixels", false, "Array"sv}, {"w", false, "Long"sv},
                           {"h", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            auto px = canvas_int_array_arg<uint32_t>(env, "pixels");
            return Value(static_cast<int64_t>(_canvas_detail::sprite_load(
                px.data(), static_cast<int64_t>(px.size()),
                env->get("w").to_long(), env->get("h").to_long())));
          },
          "Long"sv)),
      false);

  // _Canvas.sprite_from_png(data: String) -> Long (handle). Raises ValueError
  // on anything stb won't decode.
  ns.initialize("sprite_from_png",
      Value(FunctionValue({{"data", false, "String"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            auto d = culebra::image::decode_png(
                env->get("data").to_string_view());
            if (!d.error.empty())
              throw CulebraError("ValueError", d.error, line, col);
            return Value(static_cast<int64_t>(_canvas_detail::sprite_adopt(
                std::move(d.px), d.w, d.h)));
          },
          "Long"sv)),
      false);

  // _Canvas.sprite_to_png(id) -> String (PNG bytes). The inverse of
  // sprite_from_png; id 0 names the current draw target, which is what
  // Canvas.to_png() passes.
  ns.initialize("sprite_to_png",
      Value(FunctionValue({{"id", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            auto t = _canvas_detail::readback_target(env->get("id").to_long());
            if (t.px == nullptr) {
              auto r = _canvas_detail::refusal();
              throw CulebraError(r.kind, r.message, line, col);
            }
            auto e = culebra::image::encode_png(t.px, t.w, t.h);
            if (!e.error.empty())
              throw CulebraError("ValueError", e.error, line, col);
            return Value(std::move(e.data));
          },
          "String"sv)),
      false);

  // _Canvas.sprite_width(id) / sprite_height(id) -> Long (0 for an unknown
  // handle). from_png learns its size from the decode, so the preamble reads
  // it back rather than being told it.
  ns.initialize("sprite_width",
      Value(FunctionValue({{"id", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            return Value(static_cast<int64_t>(
                _canvas_detail::sprite_width(env->get("id").to_long())));
          },
          "Long"sv)),
      false);
  ns.initialize("sprite_height",
      Value(FunctionValue({{"id", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            return Value(static_cast<int64_t>(
                _canvas_detail::sprite_height(env->get("id").to_long())));
          },
          "Long"sv)),
      false);

  // _Canvas.sprite_blank(w, h, rgba) -> Long (handle; a one-colour sprite,
  // the raw material of an offscreen draw target)
  ns.initialize("sprite_blank",
      Value(FunctionValue({{"w", false, "Long"sv}, {"h", false, "Long"sv},
                           {"rgba", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            return Value(static_cast<int64_t>(_canvas_detail::sprite_blank(
                env->get("w").to_long(), env->get("h").to_long(),
                static_cast<uint32_t>(env->get("rgba").to_long()))));
          },
          "Long"sv)),
      false);

  // _Canvas.sprite_free(id) -> Nil. Raises when `id` is the current draw
  // target; an unknown/already-freed handle is a no-op.
  ns.initialize("sprite_free",
      Value(FunctionValue({{"id", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            if (!_canvas_detail::sprite_free(env->get("id").to_long()))
              throw CulebraError("ValueError",
                                 _canvas_detail::kFreeTargetError,
                                 env->get("__LINE__").to_long(),
                                 env->get("__COLUMN__").to_long());
            return Value();
          })),
      false);

  // _Canvas.target(id) -> Long (the previous target). 0 = the framebuffer;
  // raises when `id` names no live sprite.
  ns.initialize("target",
      Value(FunctionValue({{"id", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t prev = _canvas_detail::target(env->get("id").to_long());
            if (prev < 0) {
              auto r = _canvas_detail::refusal();
              throw CulebraError(r.kind, r.message,
                                 env->get("__LINE__").to_long(),
                                 env->get("__COLUMN__").to_long());
            }
            return Value(static_cast<int64_t>(prev));
          },
          "Long"sv)),
      false);

  // _Canvas.blit(id, dx, dy, sx, sy, sw, sh, flags) -> Nil. Raises when `id`
  // is the current draw target (a blit would read its own writes).
  ns.initialize("blit",
      Value(FunctionValue({{"id", false, "Long"sv},
                           {"dx", false, "Long|Float"sv},
                           {"dy", false, "Long|Float"sv},
                           {"sx", false, "Long|Float"sv},
                           {"sy", false, "Long|Float"sv},
                           {"sw", false, "Long|Float"sv},
                           {"sh", false, "Long|Float"sv},
                           {"flags", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            if (!_canvas_detail::blit(
                    env->get("id").to_long(), canvas_coord_arg(env, "dx"),
                    canvas_coord_arg(env, "dy"), canvas_coord_arg(env, "sx"),
                    canvas_coord_arg(env, "sy"), canvas_coord_arg(env, "sw"),
                    canvas_coord_arg(env, "sh"), env->get("flags").to_long()))
              throw CulebraError("ValueError", _canvas_detail::kSelfBlitError,
                                 env->get("__LINE__").to_long(),
                                 env->get("__COLUMN__").to_long());
            return Value();
          })),
      false);

  // _Canvas.blit_scaled(id, dx, dy, dw, dh, sx, sy, sw, sh, flags, alpha)
  // -> Nil (resampling blit; flags adds 8 = box-average when shrinking)
  ns.initialize("blit_scaled",
      Value(FunctionValue({{"id", false, "Long"sv},
                           {"dx", false, "Long|Float"sv},
                           {"dy", false, "Long|Float"sv},
                           {"dw", false, "Long|Float"sv},
                           {"dh", false, "Long|Float"sv},
                           {"sx", false, "Long|Float"sv},
                           {"sy", false, "Long|Float"sv},
                           {"sw", false, "Long|Float"sv},
                           {"sh", false, "Long|Float"sv},
                           {"flags", false, "Long"sv},
                           {"alpha", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            if (!_canvas_detail::blit_scaled(
                    env->get("id").to_long(), canvas_coord_arg(env, "dx"),
                    canvas_coord_arg(env, "dy"), canvas_coord_arg(env, "dw"),
                    canvas_coord_arg(env, "dh"), canvas_coord_arg(env, "sx"),
                    canvas_coord_arg(env, "sy"), canvas_coord_arg(env, "sw"),
                    canvas_coord_arg(env, "sh"), env->get("flags").to_long(),
                    env->get("alpha").to_long()))
              throw CulebraError("ValueError", _canvas_detail::kSelfBlitError,
                                 env->get("__LINE__").to_long(),
                                 env->get("__COLUMN__").to_long());
            return Value();
          })),
      false);

  // _Canvas.present() -> Nil (show the frame; suspends to the next animation
  // frame in the JSPI browser build, no-op in a declared-headless run; raises
  // when the run asked for a window that could not open).
  ns.initialize("present",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment> env) -> Value {
            _canvas_detail::present();
            if (const char* e = _canvas_detail::window_error())
              throw CulebraError("RuntimeError", e,
                                 env->get("__LINE__").to_long(),
                                 env->get("__COLUMN__").to_long());
            return Value();
          })),
      false);

  // _Canvas.buttons() -> Long (held-button bitmask; 0 headless)
  ns.initialize("buttons",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(static_cast<int64_t>(_canvas_detail::buttons()));
          },
          "Long"sv)),
      false);

  // _Canvas.mouse_x() / mouse_y() / mouse_buttons() -> Long (0 headless)
  ns.initialize("mouse_x",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(static_cast<int64_t>(_canvas_detail::mouse_x()));
          },
          "Long"sv)),
      false);
  ns.initialize("mouse_y",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(static_cast<int64_t>(_canvas_detail::mouse_y()));
          },
          "Long"sv)),
      false);
  ns.initialize("mouse_buttons",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(static_cast<int64_t>(_canvas_detail::mouse_buttons()));
          },
          "Long"sv)),
      false);

  // _Canvas.key(name: String) -> Bool. Held state of one key in Term's key
  // vocabulary (a printable character, or "left"/"enter"/"f1"/…); an unknown
  // name is simply not held. False on the headless backend.
  ns.initialize("key",
      Value(FunctionValue({{"name", false, "String"sv}},
          [](std::shared_ptr<Environment> env) {
            return Value(_canvas_detail::key(
                std::string(env->get("name").to_string_view()).c_str()));
          },
          "Bool"sv)),
      false);

  // _Canvas.key_pop() -> String. One pressed-key event (same vocabulary), ""
  // when the queue is empty. The queue is capped; oldest events fall off.
  ns.initialize("key_pop",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(_canvas_detail::key_pop());
          },
          "String"sv)),
      false);

  // _Canvas.char_pop() -> String. One typed character (text input, so shift /
  // layout / IME apply), "" when the queue is empty.
  ns.initialize("char_pop",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(_canvas_detail::char_pop());
          },
          "String"sv)),
      false);

  // _Canvas.closing() -> Bool (the window's close box was clicked; false on the
  // headless/browser backends, where the loop stops via tick()/frames)
  ns.initialize("closing",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(_canvas_detail::closing());
          },
          "Bool"sv)),
      false);

  // _Canvas.windowed() -> Bool (frames are reaching a display: true on the
  // browser, true for a window build that opened one, false headless)
  ns.initialize("windowed",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(_canvas_detail::windowed());
          },
          "Bool"sv)),
      false);

  // _Canvas.title(name) — name the window. No-op where there is none.
  ns.initialize("title",
      Value(FunctionValue({{"name", false, "String"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::set_title(
                std::string(env->get("name").to_string_view()).c_str());
            return Value();
          })),
      false);

  // _Canvas.tone(freq, dur, vol, wave) -> Nil (no-op headless)
  ns.initialize("tone",
      Value(FunctionValue({{"start_freq", false, "Long"sv},
                           {"end_freq", false, "Long"sv},
                           {"attack", false, "Long"sv},
                           {"decay", false, "Long"sv},
                           {"sustain", false, "Long"sv},
                           {"release", false, "Long"sv},
                           {"vol", false, "Long"sv}, {"peak", false, "Long"sv},
                           {"channel", false, "Long"sv},
                           {"duty", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::tone(
                env->get("start_freq").to_long(),
                env->get("end_freq").to_long(), env->get("attack").to_long(),
                env->get("decay").to_long(), env->get("sustain").to_long(),
                env->get("release").to_long(), env->get("vol").to_long(),
                env->get("peak").to_long(), env->get("channel").to_long(),
                env->get("duty").to_long());
            return Value();
          })),
      false);

  // _Canvas.music_play(data: String, loop, vol, start) -> Nil. The format
  // sniff (MP3/Ogg) runs here, before any backend branch, so unplayable bytes
  // raise the same ValueError at the same site on every backend — playable,
  // headless or browser alike. Everything past the sniff clamps or no-ops.
  ns.initialize("music_play",
      Value(FunctionValue({{"data", false, "String"sv},
                           {"loop", false, "Long"sv}, {"vol", false, "Long"sv},
                           {"start", false, "Long|Float"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            auto sv = env->get("data").to_string_view();
            auto p = reinterpret_cast<const uint8_t*>(sv.data());
            const char* fmt = _canvas_detail::music_format(p, sv.size());
            if (fmt == nullptr)
              throw CulebraError("ValueError",
                                 _canvas_detail::kMusicFormatError, line, col);
            _canvas_detail::music_play(
                p, static_cast<int64_t>(sv.size()), fmt,
                env->get("loop").to_long(), env->get("vol").to_long(),
                env->get("start").to_double_coerce());
            return Value();
          })),
      false);

  // _Canvas.music_stop() / music_pause() / music_resume() -> Nil (no-op when
  // nothing is loaded)
  ns.initialize("music_stop",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            _canvas_detail::music_stop();
            return Value();
          })),
      false);
  ns.initialize("music_pause",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            _canvas_detail::music_pause();
            return Value();
          })),
      false);
  ns.initialize("music_resume",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            _canvas_detail::music_resume();
            return Value();
          })),
      false);

  // _Canvas.music_volume(vol) -> Nil (0..100, the tone scale)
  ns.initialize("music_volume",
      Value(FunctionValue({{"vol", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::music_volume(env->get("vol").to_long());
            return Value();
          })),
      false);

  // _Canvas.music_seek(seconds) -> Nil
  ns.initialize("music_seek",
      Value(FunctionValue({{"seconds", false, "Long|Float"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::music_seek(
                env->get("seconds").to_double_coerce());
            return Value();
          })),
      false);

  // _Canvas.music_playing() -> Bool (false when nothing is loaded / headless)
  ns.initialize("music_playing",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(_canvas_detail::music_playing());
          },
          "Bool"sv)),
      false);

  // _Canvas.sound_load(data: String) -> Long (handle). WAV joins music's
  // MP3/Ogg; the sniff runs here so unplayable bytes raise the same
  // ValueError at the same site on every backend. Undecodable-past-the-sniff
  // stays silent, like music.
  ns.initialize("sound_load",
      Value(FunctionValue({{"data", false, "String"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            auto sv = env->get("data").to_string_view();
            auto p = reinterpret_cast<const uint8_t*>(sv.data());
            const char* fmt = _canvas_detail::sound_format(p, sv.size());
            if (fmt == nullptr)
              throw CulebraError("ValueError",
                                 _canvas_detail::kSoundFormatError,
                                 env->get("__LINE__").to_long(),
                                 env->get("__COLUMN__").to_long());
            int64_t id = _canvas_detail::sound_alloc_id();
            _canvas_detail::sound_load(id, p, static_cast<int64_t>(sv.size()),
                                       fmt);
            return Value(static_cast<int64_t>(id));
          },
          "Long"sv)),
      false);

  // _Canvas.sound_play(id, vol) / sound_stop(id) / sound_free(id) -> Nil,
  // _Canvas.sound_playing(id) -> Bool. All no-ops / false for an unknown
  // handle or on the headless backend.
  ns.initialize("sound_play",
      Value(FunctionValue({{"id", false, "Long"sv}, {"vol", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::sound_play(env->get("id").to_long(),
                                       env->get("vol").to_long());
            return Value();
          })),
      false);
  ns.initialize("sound_stop",
      Value(FunctionValue({{"id", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::sound_stop(env->get("id").to_long());
            return Value();
          })),
      false);
  ns.initialize("sound_playing",
      Value(FunctionValue({{"id", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            return Value(_canvas_detail::sound_playing(
                env->get("id").to_long()));
          },
          "Bool"sv)),
      false);
  ns.initialize("sound_free",
      Value(FunctionValue({{"id", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            _canvas_detail::sound_free(env->get("id").to_long());
            return Value();
          })),
      false);

  // _Canvas.width() / height() -> Long (current framebuffer dimensions)
  ns.initialize("width",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(static_cast<int64_t>(_canvas_detail::width()));
          },
          "Long"sv)),
      false);
  ns.initialize("height",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(static_cast<int64_t>(_canvas_detail::height()));
          },
          "Long"sv)),
      false);

  return Value(std::move(ns));
}

inline Value make_random_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  auto get_num = [](std::shared_ptr<Environment>& env, const char* name) {
    const auto& v = env->get(name);
    if (!v.is_numeric()) {
      auto line = env->get("__LINE__").to_long();
      auto col = env->get("__COLUMN__").to_long();
      throw_type_error_at(line, col);
    }
    return v.to_double_coerce();
  };

  ns.initialize(
      "seed",
      Value(FunctionValue({{"n", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto n = env->get("n").to_long();
                            random_engine().seed(
                                static_cast<uint64_t>(n));
                            return Value();
                          })),
      false);

  ns.initialize(
      "int",
      Value(FunctionValue(
          {{"lo", false, "Long"sv}, {"hi", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            auto lo = env->get("lo").to_long();
            auto hi = env->get("hi").to_long();
            if (hi <= lo) {
              auto line = env->get("__LINE__").to_long();
              auto col = env->get("__COLUMN__").to_long();
              throw_type_error_at(line, col);
            }
            std::uniform_int_distribution<int64_t> d(lo, hi - 1);
            return Value(d(random_engine()));
          },
          "Long"sv)),
      false);

  ns.initialize(
      "uniform",
      Value(FunctionValue(
          {{"lo", false}, {"hi", false}},
          [get_num](std::shared_ptr<Environment> env) {
            auto lo = get_num(env, "lo");
            auto hi = get_num(env, "hi");
            std::uniform_real_distribution<double> d(lo, hi);
            return Value(d(random_engine()));
          },
          "Float"sv)),
      false);

  ns.initialize(
      "gauss",
      Value(FunctionValue(
          {{"mu", false}, {"sigma", false}},
          [get_num](std::shared_ptr<Environment> env) {
            auto mu = get_num(env, "mu");
            auto sigma = get_num(env, "sigma");
            std::normal_distribution<double> d(mu, sigma);
            return Value(d(random_engine()));
          },
          "Float"sv)),
      false);

  ns.initialize(
      "shuffle",
      Value(FunctionValue(
          {{"a", false, "Array"sv}},
          [](std::shared_ptr<Environment> env) {
            auto& arr = *env->get("a").to_array().values;
            std::shuffle(arr.begin(), arr.end(), random_engine());
            return Value();
          })),
      false);

  ns.initialize(
      "weighted_choice",
      Value(FunctionValue(
          {{"pop", false, "Array"sv}, {"weights", false, "Array"sv}},
          [](std::shared_ptr<Environment> env) {
            const auto& pop = *env->get("pop").to_array().values;
            const auto& weights = *env->get("weights").to_array().values;
            if (pop.empty() || pop.size() != weights.size()) {
              auto line = env->get("__LINE__").to_long();
              auto col = env->get("__COLUMN__").to_long();
              throw_type_error_at(line, col);
            }
            // Reused between calls to avoid a heap allocation per draw
            // (the microgpt inference path calls this per token).
            thread_local std::vector<double> scratch;
            scratch.clear();
            scratch.reserve(weights.size());
            for (const auto& v : weights) {
              if (!v.is_numeric()) {
                auto line = env->get("__LINE__").to_long();
                auto col = env->get("__COLUMN__").to_long();
                throw_type_error_at(line, col);
              }
              scratch.push_back(v.to_double_coerce());
            }
            std::discrete_distribution<size_t> d(scratch.begin(),
                                                 scratch.end());
            return pop[d(random_engine())];
          })),
      false);

  return Value(std::move(ns));
}

// Tensor.zeros(3, 4) and Tensor.zeros([3, 4]) produce the same shape.
// Reads positional Long entries from `args[offset..]`, or unwraps a
// single Array argument if that is what's there.
inline TensorShape parse_tensor_shape(const std::vector<Value>& args,
                                      size_t offset, size_t line, size_t col) {
  std::vector<int64_t> dims;
  auto push_long = [&](const Value& v) {
    if (v.type != Value::Long) throw_type_error_at(line, col);
    dims.push_back(v.to_long());
  };
  if (args.size() - offset == 1 && args[offset].type == Value::Array) {
    for (const auto& v : *args[offset].to_array().values) push_long(v);
  } else {
    for (size_t i = offset; i < args.size(); i++) push_long(args[i]);
  }
  // TensorShape ctor rejects negative dims.
  return TensorShape(std::move(dims));
}

// If args[0] is a "f32" tag, consume it; otherwise default F32.
// Returns {dtype, offset of first shape arg}.
inline std::pair<Dtype, size_t> parse_tensor_dtype_prefix(
    const std::vector<Value>& args, size_t line, size_t col) {
  if (args.empty() || args[0].type != Value::String) return {Dtype::F32, 0};
  auto d = parse_dtype(args[0].get<std::string>());
  if (!d) throw_type_error_at(line, col);
  return {*d, 1};
}

template <typename T>
inline void _tensor_fill_1d(T* out, const std::vector<Value>& vs, size_t line,
                            size_t col) {
  for (size_t i = 0; i < vs.size(); i++) {
    if (!vs[i].is_numeric()) throw_type_error_at(line, col);
    out[i] = static_cast<T>(vs[i].to_double_coerce());
  }
}

template <typename T>
inline void _tensor_fill_2d(T* out, const std::vector<Value>& rows_v,
                            size_t cols, size_t line, size_t col) {
  for (size_t i = 0; i < rows_v.size(); i++) {
    if (rows_v[i].type != Value::Array) throw_type_error_at(line, col);
    const auto& row = *rows_v[i].to_array().values;
    if (row.size() != cols) throw_type_error_at(line, col);
    for (size_t j = 0; j < cols; j++) {
      if (!row[j].is_numeric()) throw_type_error_at(line, col);
      out[i * cols + j] = static_cast<T>(row[j].to_double_coerce());
    }
  }
}

// Detects 1D Array<Float|Long> or 2D Array of equal-length numeric
// rows. Higher-rank "from" is deferred to M2+.
inline TensorPtr tensor_from_array(const ArrayValue& a, Dtype dt, size_t line,
                                   size_t col) {
  const auto& vs = *a.values;
  if (vs.empty()) return tensor_zeros(TensorShape({0}), dt);

  if (vs[0].is_numeric()) {
    auto t = std::make_shared<TensorImpl>(
        TensorShape({static_cast<int64_t>(vs.size())}), dt);
    _tensor_fill_1d(t->data_as<float>(), vs, line, col);
    return t;
  }

  if (vs[0].type != Value::Array) throw_type_error_at(line, col);
  size_t cols = vs[0].to_array().values->size();
  size_t rows = vs.size();
  auto t = std::make_shared<TensorImpl>(
      TensorShape({static_cast<int64_t>(rows), static_cast<int64_t>(cols)}),
      dt);
  _tensor_fill_2d(t->data_as<float>(), vs, cols, line, col);
  return t;
}

inline Value make_tensor_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  // Variadic ctor: declared `*args` so every positional arg lands in __ARGS__
  // and the arity stays variadic (an empty formal list now reads as strict
  // 0-arity). Layout is [optional dtype string, shape varargs OR single Array].
  auto make_ctor = [](TensorPtr (*kernel)(TensorShape, Dtype)) {
    return Value(FunctionValue(
        {FunctionValue::Parameter::make_args_rest("args")},
        [kernel](std::shared_ptr<Environment> env) {
          auto line = env->get("__LINE__").to_long();
          auto col = env->get("__COLUMN__").to_long();
          if (!env->has("__ARGS__")) throw_type_error_at(line, col);
          const auto& args = *env->get("__ARGS__").to_array().values;
          auto [dt, offset] = parse_tensor_dtype_prefix(args, line, col);
          auto shape = parse_tensor_shape(args, offset, line, col);
          return Value(TensorValue(kernel(std::move(shape), dt)));
        },
        "Tensor"sv));
  };

  ns.initialize("zeros", make_ctor(&tensor_zeros), false);
  ns.initialize("ones", make_ctor(&tensor_ones), false);
  ns.initialize("randn", make_ctor(&tensor_randn), false);

  // Activations (relu / sigmoid / softmax) are Tensor instance methods
  // — `t.relu()` — registered in TensorValue::builtins(). A user class
  // may define its own `relu` etc. without collision: method lookup
  // gives the class method priority on every backend.

  ns.initialize(
      "eval",
      Value(FunctionValue(
          {FunctionValue::Parameter::make_args_rest("args")},
          [](std::shared_ptr<Environment> env) {
            auto line = env->get("__LINE__").to_long();
            auto col = env->get("__COLUMN__").to_long();
            if (!env->has("__ARGS__")) return Value();
            const auto& args = *env->get("__ARGS__").to_array().values;
            for (const auto& v : args) {
              // Match the JIT's descriptive message (Tensor.eval is variadic,
              // so there's no single typed param to name); both report at the
              // call site.
              if (v.type != Value::Tensor)
                throw CulebraError(
                    "TypeError",
                    "type error: Tensor.eval argument expects Tensor", line,
                    col);
              tensor_eval_node(*v.to_tensor().impl);
            }
            return Value();
          })),
      false);

  ns.initialize(
      "from_csv",
      Value(FunctionValue(
          {{"path", false, "String"sv}},
          [](std::shared_ptr<Environment> env) {
            const auto& path = env->get("path").to_string();
            return Value(TensorValue(tensor_from_csv(path, Dtype::F32)));
          },
          "Tensor"sv)),
      false);

  ns.initialize(
      "from",
      Value(FunctionValue(
          {{"a", false, "Array"sv}},
          [](std::shared_ptr<Environment> env) {
            auto line = env->get("__LINE__").to_long();
            auto col = env->get("__COLUMN__").to_long();
            const auto& a = env->get("a").to_array();
            // No dtype arg in M1: always F32. M2+ may take a trailing
            // string tag once mixed-dtype binops are sorted.
            auto t = tensor_from_array(a, Dtype::F32, line, col);
            return Value(TensorValue(std::move(t)));
          },
          "Tensor"sv)),
      false);

  // Tensor.concat([a, b, ...]) stacks tensors along axis 0 (rows).
  ns.initialize(
      "concat",
      Value(FunctionValue(
          {{"parts", false, "Array"sv}},
          [](std::shared_ptr<Environment> env) {
            auto line = env->get("__LINE__").to_long();
            auto col = env->get("__COLUMN__").to_long();
            const auto& arr = *env->get("parts").to_array().values;
            std::vector<TensorPtr> parts;
            parts.reserve(arr.size());
            for (const auto& v : arr) {
              // Match the JIT runtime's generic message (throw_type_error_at)
              // for a non-Tensor element, same as Tensor.from.
              if (v.type != Value::Tensor) throw_type_error_at(line, col);
              parts.push_back(v.to_tensor().impl);
            }
            return Value(TensorValue(tensor_concat(std::move(parts))));
          },
          "Tensor"sv)),
      false);

  // Tensor.no_grad(fn): run `fn` with autograd tracking suppressed, so
  // ops inside build no grad graph (inference). Returns fn's result. The
  // RAII guard restores tracking even if fn throws.
  ns.initialize(
      "no_grad",
      Value(FunctionValue(
          {{"fn", false, "Function"sv}},
          [](std::shared_ptr<Environment> env) {
            const auto& fn = env->get("fn");
            TensorNoGradGuard guard;
            return _invoke_callback(fn);
          },
          "Any"sv)),
      false);

  // Device selection: which backend evaluates tensor ops. Process-global
  // (a single tl device enum shared by interp and JIT). use_auto picks per
  // op; gpu_available() reports whether a GPU backend is present at runtime
  // (Metal on macOS; false where none is compiled in / reachable).
  ns.initialize(
      "use_cpu",
      Value(FunctionValue({}, [](std::shared_ptr<Environment>) {
        tensor_use_cpu();
        return Value();
      })),
      false);
  ns.initialize(
      "use_gpu",
      Value(FunctionValue({}, [](std::shared_ptr<Environment>) {
        tensor_use_gpu();
        return Value();
      })),
      false);
  ns.initialize(
      "use_auto",
      Value(FunctionValue({}, [](std::shared_ptr<Environment>) {
        tensor_use_auto();
        return Value();
      })),
      false);
  ns.initialize(
      "gpu_available",
      Value(FunctionValue(
          {},
          [](std::shared_ptr<Environment>) {
            return Value(tensor_gpu_available());
          },
          "Bool"sv)),
      false);
  ns.initialize(
      "device",
      Value(FunctionValue(
          {},
          [](std::shared_ptr<Environment>) {
            return Value(std::string(tensor_device()));
          },
          "String"sv)),
      false);

  return Value(std::move(ns));
}

inline Value make_sys_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  ArrayValue arr;
  const auto& argv = culebra::sys_argv();
  arr.values->reserve(argv.size());
  for (const auto& s : argv) {
    arr.values->push_back(Value(std::string(s)));
  }
  ns.initialize("argv", Value(std::move(arr)), false);

  // Absolute path to the running culebra binary, for re-spawning a worker copy
  // of the interpreter (e.g. Proc.run([Sys.executable, "worker.cul"], ...)).
  ns.initialize("executable", Value(culebra::current_executable_path()), false);

  // Absolute path of the running script (the `__file__` analogue), or nil when
  // there is no source file — the REPL, stdin, or an AOT binary. Lets a script
  // resolve files next to itself instead of relying on the cwd.
  ns.initialize("script",
                main_script_path().empty()
                    ? Value()
                    : Value(std::string(main_script_path())),
                false);

  ns.initialize(
      "exit",
      Value(FunctionValue({{"code", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto code = env->get("code").to_long();
                            std::exit(static_cast<int>(code));
                            return Value();  // unreachable
                          })),
      false);

  // `fallback` is returned unchanged (any type) when the variable is unset, so
  // `Sys.env('X', nil)` tells "unset" apart from "set to the empty string" —
  // the default of "" keeps the one-argument form returning a String.
  ns.initialize(
      "env",
      Value(FunctionValue(
          {{"name", false, "String"sv},
           {"fallback", false, ""sv, nullptr, kw_default_empty_str()}},
          [](std::shared_ptr<Environment> env) {
            const auto& name = env->get("name").to_string();
            const char* v = std::getenv(name.c_str());
            if (v) return Value(std::string(v));
            return env->get("fallback");
          })),
      false);

  // Current working directory. IOError on failure (e.g. the cwd was
  // removed out from under the process).
  ns.initialize(
      "getcwd",
      Value(FunctionValue({},
                          [](std::shared_ptr<Environment> env) {
                            int64_t line = env->get("__LINE__").to_long();
                            int64_t col = env->get("__COLUMN__").to_long();
                            std::error_code ec;
                            auto p = std::filesystem::current_path(ec);
                            if (ec) _io_throw("Sys.getcwd", line, col, ec);
                            return Value(p.string());
                          },
                          "String"sv)),
      false);

  // Change the process working directory. IOError if the path is missing or
  // not a directory.
  ns.initialize(
      "chdir",
      Value(FunctionValue({{"path", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) {
                            int64_t line = env->get("__LINE__").to_long();
                            int64_t col = env->get("__COLUMN__").to_long();
                            const auto& p = env->get("path").to_string();
                            std::error_code ec;
                            std::filesystem::current_path(
                                std::filesystem::path(p), ec);
                            if (ec) {
                              _io_throw(std::format("Sys.chdir('{}')", p),
                                        line, col, ec);
                            }
                            return Value();
                          })),
      false);

  // Set an environment variable (overwrites). Visible to this process and to
  // children spawned afterwards (e.g. Proc.run). IOError on failure.
  ns.initialize(
      "set_env",
      Value(FunctionValue(
          {{"name", false, "String"sv}, {"value", false, "String"sv}},
          [](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& name = env->get("name").to_string();
            const auto& value = env->get("value").to_string();
            if (os_setenv(name.c_str(), value.c_str(), 1) != 0) {
              _io_throw(std::format("Sys.set_env('{}')", name), line, col,
                        std::error_code(errno, std::generic_category()));
            }
            return Value();
          })),
      false);

  // Monotonic seconds since first call. Anchor at process startup so
  // time differences across calls are measured against a stable origin.
  ns.initialize(
      "time",
      Value(FunctionValue({},
                          [](std::shared_ptr<Environment>) {
                            using clock = std::chrono::steady_clock;
                            static const auto t0 = clock::now();
                            auto now = clock::now();
                            return Value(std::chrono::duration<double>(
                                             now - t0)
                                             .count());
                          },
                          "Float"sv)),
      false);

  return Value(std::move(ns));
}

// GC introspection. `GC.stat()` returns an Object with `live_objects` (net
// live refcounted heap objects = births − frees, deterministic) and
// `heap_bytes` (those objects' struct sizes summed — element buffers and
// allocator overhead excluded, so a structural approximation). Snapshot the
// counters before building the result Object so the container isn't counted
// in its own report.
inline Value make_gc_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize(
      "stat",
      Value(FunctionValue({},
                          [](std::shared_ptr<Environment>) {
                            auto& gc = interp_gc();
                            gc.collect();  // report reachable, not cycle residue
                            int64_t live = static_cast<int64_t>(gc.live_objects);
                            int64_t bytes = static_cast<int64_t>(gc.live_bytes);
                            ObjectValue stat;
                            stat.initialize("live_objects", Value(live), false);
                            // Symmetry with the JIT: refcounted-only live count.
                            // The interp has no traced-only values (every heap
                            // object is shared_ptr-managed), so it equals
                            // live_objects; the RC-leak fuzzer reads this field
                            // on both backends.
                            stat.initialize("rc_objects", Value(live), false);
                            stat.initialize("heap_bytes", Value(bytes), false);
                            return Value(std::move(stat));
                          },
                          "Object"sv)),
      false);
  return Value(std::move(ns));
}

// Built by make_shared_buffer_handle in isolate.h (next to the channel
// endpoint + Sendable hooks, where sendable.h is available). Forward-declared
// here so SharedBuffer.new can hand back a proper handle.
inline Value make_shared_buffer_handle(int64_t id, int64_t count);

// `SharedBuffer.new(count, Cls)` allocates a flat, zero-initialized byte
// store holding `count` records laid out per the @packable class `Cls`,
// and returns a buffer handle. `buf[i]` yields a packed view whose
// `.field` reads/writes the backing bytes directly (zero copy). The
// native storage lives in a global registry; the handle just carries its
// integer id (a Value can't hold a raw shared_ptr).
inline Value make_shared_buffer_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize(
      "new",
      Value(FunctionValue(
          {{"count", false, ""sv}, {"type", false, ""sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line =
                env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            int64_t col =
                env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
            int64_t count = env->get("count").to_long();
            const Value& tv = env->get("type");
            if (count < 0) {
              throw culebra::CulebraError(
                  "ValueError", "SharedBuffer.new: count must be >= 0", line,
                  col);
            }
            if (tv.type != Value::Object || !tv.to_object().has("__packable__")) {
              throw culebra::CulebraError(
                  "TypeError",
                  "SharedBuffer.new: second argument must be a @packable class",
                  line, col);
            }
            const auto& cls = tv.to_object().get("__packable__").get<std::string>();
            const auto* layout = culebra::lookup_packable_layout(cls);
            if (!layout) {
              throw culebra::CulebraError(
                  "TypeError",
                  std::format("SharedBuffer.new: no @packable layout for `{}`",
                              cls),
                  line, col);
            }
            int64_t id = culebra::make_shared_buffer(
                *layout, cls, static_cast<size_t>(count));
            return make_shared_buffer_handle(id, count);
          },
          "Object"sv)),
      false);
  // `SharedBuffer.file(path, count, Cls)` — file-backed (mmap), persistent.
  // Same buffer interface; the handle additionally exposes `flush()`.
  ns.initialize(
      "file",
      Value(FunctionValue(
          {{"path", false, ""sv}, {"count", false, ""sv}, {"type", false, ""sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line =
                env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            int64_t col =
                env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
            const Value& pathv = env->get("path");
            if (!pathv.is_stringlike()) {
              throw culebra::CulebraError(
                  "TypeError", "SharedBuffer.file: path must be a String", line,
                  col);
            }
            std::string path(pathv.to_string_view());
            int64_t count = env->get("count").to_long();
            const Value& tv = env->get("type");
            if (count < 0) {
              throw culebra::CulebraError(
                  "ValueError", "SharedBuffer.file: count must be >= 0", line,
                  col);
            }
            if (tv.type != Value::Object || !tv.to_object().has("__packable__")) {
              throw culebra::CulebraError(
                  "TypeError",
                  "SharedBuffer.file: type must be a @packable class", line, col);
            }
            const auto& cls = tv.to_object().get("__packable__").get<std::string>();
            const auto* layout = culebra::lookup_packable_layout(cls);
            if (!layout) {
              throw culebra::CulebraError(
                  "TypeError",
                  std::format("SharedBuffer.file: no @packable layout for `{}`",
                              cls),
                  line, col);
            }
            int64_t id = culebra::make_shared_buffer_file(
                *layout, cls, static_cast<size_t>(count), path);
            return make_shared_buffer_handle(id, count);
          },
          "Object"sv)),
      false);
  // `SharedBuffer.shared(count, Cls)` — anonymous fd-backed RAM, shareable with
  // a child process via Proc.run `share:`. Same interface as `new`; the handle
  // additionally carries a fd that the child inherits.
  ns.initialize(
      "shared",
      Value(FunctionValue(
          {{"count", false, ""sv}, {"type", false, ""sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line =
                env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            int64_t col =
                env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
            int64_t count = env->get("count").to_long();
            const Value& tv = env->get("type");
            if (count < 0) {
              throw culebra::CulebraError(
                  "ValueError", "SharedBuffer.shared: count must be >= 0", line,
                  col);
            }
            if (tv.type != Value::Object || !tv.to_object().has("__packable__")) {
              throw culebra::CulebraError(
                  "TypeError",
                  "SharedBuffer.shared: second argument must be a @packable "
                  "class",
                  line, col);
            }
            const auto& cls = tv.to_object().get("__packable__").get<std::string>();
            const auto* layout = culebra::lookup_packable_layout(cls);
            if (!layout) {
              throw culebra::CulebraError(
                  "TypeError",
                  std::format("SharedBuffer.shared: no @packable layout for `{}`",
                              cls),
                  line, col);
            }
            int64_t id = culebra::make_shared_buffer_shared(
                *layout, cls, static_cast<size_t>(count));
            return make_shared_buffer_handle(id, count);
          },
          "Object"sv)),
      false);
  // `SharedBuffer.receive(name, Cls)` — child side: mmap the buffer the parent
  // passed via Proc.run `share:`. `count` is the parent's (read from the
  // environment); the child only names the buffer + its @packable type.
  ns.initialize(
      "receive",
      Value(FunctionValue(
          {{"name", false, ""sv}, {"type", false, ""sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line =
                env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            int64_t col =
                env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
            const Value& namev = env->get("name");
            if (!namev.is_stringlike()) {
              throw culebra::CulebraError(
                  "TypeError", "SharedBuffer.receive: name must be a String",
                  line, col);
            }
            const Value& tv = env->get("type");
            if (tv.type != Value::Object || !tv.to_object().has("__packable__")) {
              throw culebra::CulebraError(
                  "TypeError",
                  "SharedBuffer.receive: second argument must be a @packable "
                  "class",
                  line, col);
            }
            const auto& cls = tv.to_object().get("__packable__").get<std::string>();
            const auto* layout = culebra::lookup_packable_layout(cls);
            if (!layout) {
              throw culebra::CulebraError(
                  "TypeError",
                  std::format("SharedBuffer.receive: no @packable layout for `{}`",
                              cls),
                  line, col);
            }
            int64_t id = culebra::make_shared_buffer_from_share_env(
                *layout, cls, namev.to_string_view());
            auto core = culebra::lookup_shared_buffer(id);
            return make_shared_buffer_handle(
                id, core ? static_cast<int64_t>(core->count) : 0);
          },
          "Object"sv)),
      false);
  return Value(std::move(ns));
}

// Build the `{code, stdout, stderr, ok, signal}` result Object shared by
// the interp Proc.run lambda. `signal` is nil unless the child was killed.
// Build the `{code, stdout, stderr, ok, signal, error, timed_out}` result
// Object shared by Proc.run/all/race. A spawned outcome carries the process
// result with `error` nil; a spawn failure carries `ok:false` and the failure
// message in `error` (Proc.all's allSettled error representation).
inline Value proc_outcome_to_value(culebra::proc::RunOutcome&& oc) {
  ObjectValue obj;
  if (oc.spawned) {
    auto& r = oc.result;
    obj.initialize("code", Value(r.code), false);
    obj.initialize("stdout", Value(std::move(r.out)), false);
    obj.initialize("stderr", Value(std::move(r.err)), false);
    obj.initialize("ok", Value(r.ok), false);
    obj.initialize("signal",
                   r.signal.empty() ? Value() : Value(std::move(r.signal)),
                   false);
    obj.initialize("error", Value(), false);
    obj.initialize("timed_out", Value(r.timed_out), false);
  } else {
    obj.initialize("code", Value(static_cast<int64_t>(-1)), false);
    obj.initialize("stdout", Value(std::string("")), false);
    obj.initialize("stderr", Value(std::string("")), false);
    obj.initialize("ok", Value(false), false);
    obj.initialize("signal", Value(), false);
    obj.initialize("error",
        Value(std::format("{} failed: {}", oc.err_what,
                          std::system_category().message(oc.err_no))),
        false);
    obj.initialize("timed_out", Value(false), false);
  }
  return Value(std::move(obj));
}

// Validate and convert an Array of command Arrays into argv vectors. Used by
// Proc.all / Proc.race. Each element must be a non-empty Array<String>.
inline std::vector<std::vector<std::string>> proc_parse_command_list(
    const std::vector<Value>& outer, const char* ctx, int64_t line, int64_t col) {
  std::vector<std::vector<std::string>> commands;
  commands.reserve(outer.size());
  for (const auto& cv : outer) {
    if (cv.type != Value::Array) {
      throw CulebraError("TypeError",
          std::format("{}: each command must be an Array of String", ctx),
          line, col);
    }
    const auto& inner = *cv.to_array().values;
    if (inner.empty()) {
      throw CulebraError("ValueError",
          std::format("{}: empty command", ctx), line, col);
    }
    std::vector<std::string> argv;
    argv.reserve(inner.size());
    for (const auto& e : inner) {
      if (e.type != Value::String && e.type != Value::StringView) {
        throw CulebraError("TypeError",
            std::format("{}: command elements must be String", ctx),
            line, col);
      }
      argv.emplace_back(e.to_string_view());
    }
    commands.push_back(std::move(argv));
  }
  return commands;
}

// Parsed cmd / cwd / env / stdin shared by Proc.run and Proc.spawn.
struct ProcLaunchArgs {
  std::vector<std::string> argv;
  std::string cwd_str;
  bool has_cwd = false;
  std::vector<std::pair<std::string, std::string>> overrides;
  bool has_env = false;
  std::string stdin_data;
  std::vector<int> share_fds;  // SharedBuffer.shared fds the child inherits.
  const std::string* cwd_ptr() const { return has_cwd ? &cwd_str : nullptr; }
  const std::vector<std::pair<std::string, std::string>>* env_ptr() const {
    // The share buffers add CULEBRA_SHARE_* entries to `overrides`, so a custom
    // env exists whenever the user gave `env:` OR `share:`.
    return (has_env || !share_fds.empty()) ? &overrides : nullptr;
  }
  const std::vector<int>* share_ptr() const {
    return share_fds.empty() ? nullptr : &share_fds;
  }
};

// Parse a `share: {name: SharedBuffer.shared}` kwarg into env overrides
// (one `CULEBRA_SHARE_<name>` entry each) plus the fds the children inherit.
// Shared by Proc.run/spawn (via proc_parse_launch) and Proc.all/race. A nil
// share is a no-op; only `SharedBuffer.shared(...)` buffers may cross.
inline void proc_parse_share(
    const Value& share_v, std::string_view ctx, int64_t line, int64_t col,
    std::vector<std::pair<std::string, std::string>>& env_out,
    std::vector<int>& fds_out) {
  if (share_v.type == Value::Nil) return;
  if (share_v.type != Value::Object) {
    throw CulebraError("TypeError",
        std::format("{}: share must be an Object of name -> SharedBuffer", ctx),
        line, col);
  }
  for (const auto& [k, sym] : *share_v.to_object().properties) {
    if (sym.val.type != Value::Object ||
        !sym.val.to_object().has("__sharedbuffer_id__")) {
      throw CulebraError("TypeError",
          std::format("{}: share `{}` must be a SharedBuffer", ctx, k),
          line, col);
    }
    int64_t id = sym.val.to_object().get("__sharedbuffer_id__").to_long();
    auto [fd, env_val] = culebra::prepare_share_buffer(id, k);
    env_out.emplace_back(culebra::share_env_key(k), std::move(env_val));
    fds_out.push_back(fd);
  }
}

// Validate and collect the cmd (non-empty Array<String>), cwd (nil/String),
// env (nil/Object of String) and stdin (String) kwargs. `ctx` tags errors.
inline ProcLaunchArgs proc_parse_launch(
    const std::shared_ptr<Environment>& env, const char* ctx, int64_t line,
    int64_t col) {
  ProcLaunchArgs la;
  const auto& arr = *env->get("cmd").to_array().values;
  if (arr.empty()) {
    throw CulebraError("ValueError",
        std::format("{}: empty command", ctx), line, col);
  }
  la.argv.reserve(arr.size());
  for (const auto& v : arr) {
    if (v.type != Value::String && v.type != Value::StringView) {
      throw CulebraError("TypeError",
          std::format("{}: command elements must be String", ctx), line, col);
    }
    la.argv.emplace_back(v.to_string_view());
  }
  // cwd (String?) / env (Object?) / stdin (String) carry type annotations,
  // so the binder already rejected wrong types with the shared
  // `parameter '<name>' expects <Type>` message — only the nil-vs-present
  // dispatch and the per-value env String check remain here.
  const auto& cwd_v = env->get("cwd");
  if (cwd_v.type != Value::Nil) {
    la.cwd_str = cwd_v.to_string_view();
    la.has_cwd = true;
  }
  const auto& env_v = env->get("env");
  if (env_v.type != Value::Nil) {
    for (const auto& [k, sym] : *env_v.to_object().properties) {
      if (sym.val.type != Value::String && sym.val.type != Value::StringView) {
        throw CulebraError("TypeError",
            std::format("{}: env values must be String", ctx), line, col);
      }
      la.overrides.emplace_back(std::string(k),
                                std::string(sym.val.to_string_view()));
    }
    la.has_env = true;
  }
  la.stdin_data = env->get("stdin").to_string_view();
  // `share: {name: buf}` — anonymous SharedBuffer.shared(...) buffers the child
  // inherits by fd. Each becomes a CULEBRA_SHARE_<name> env entry the child's
  // SharedBuffer.receive reads, plus an fd the child un-CLOEXECs.
  proc_parse_share(env->get("share"), ctx, line, col, la.overrides,
                   la.share_fds);
  return la;
}

// Wrap a finished ProcResult as the standard `{code,...,timed_out}` Object.
inline Value proc_result_to_value(culebra::proc::ProcResult&& pr) {
  culebra::proc::RunOutcome oc;
  oc.spawned = true;
  oc.result = std::move(pr);
  return proc_outcome_to_value(std::move(oc));
}

// Mutate a handle field. Objects share their property map, so this is visible
// to every holder of the handle (the user's variable and the bound `self`).
inline void _proc_handle_set(const Value& this_v, std::string_view key,
                             Value val) {
  this_v.to_object().properties->insert_or_assign(
      key, Symbol{std::move(val), true});
}

// Build a File handle: carries `_id` (Long) into the side table, plus the
// Reader/Writer/Seekable/Closeable method set. `drop` (GC backstop) and
// `close` both erase the table entry (idempotent).
inline Value make_file_handle(int64_t id) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("_id", Value(static_cast<int64_t>(id)), false);
  // A native handle (the fd lives in a process-local table) is not Sendable —
  // reject it at the serialize boundary the same way the JIT handle does.
  h.initialize("__nonsendable__", Value(true), false);

  auto hid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("self").to_object().get("_id").to_long();
  };
  auto loc = [](const std::shared_ptr<Environment>& env, int64_t& line,
                int64_t& col) {
    line = env->get("__LINE__").to_long();
    col = env->get("__COLUMN__").to_long();
  };

  // read(n: Long? = nil) — streaming. nil → rest of file, Long → ≤ n bytes.
  h.initialize(
      "read",
      Value(FunctionValue({{"n", false, ""sv, nullptr, kw_default_nil()}},
                          [hid, loc](std::shared_ptr<Environment> env) {
                            int64_t line, col; loc(env, line, col);
                            const auto& n = env->get("n");
                            if (n.type == Value::Nil) {
                              return Value(_file_read_all(hid(env), line, col));
                            }
                            return Value(_file_read_n(hid(env), n.to_long(),
                                                      line, col));
                          },
                          "String"sv)),
      false);

  h.initialize(
      "write",
      Value(FunctionValue({{"data", false, "String"sv}},
                          [hid, loc](std::shared_ptr<Environment> env) {
                            int64_t line, col; loc(env, line, col);
                            _file_write(hid(env), env->get("data").to_string(),
                                        line, col);
                            return Value();
                          })),
      false);

  h.initialize(
      "flush",
      Value(FunctionValue({}, [hid, loc](std::shared_ptr<Environment> env) {
        int64_t line, col; loc(env, line, col);
        _file_flush(hid(env), line, col);
        return Value();
      })),
      false);

  h.initialize(
      "seek",
      Value(FunctionValue(
          {{"offset", false, "Long"sv},
           {"whence", false, ""sv, nullptr,
            std::make_shared<Value>(std::string("set"))}},
          [hid, loc](std::shared_ptr<Environment> env) {
            int64_t line, col; loc(env, line, col);
            _file_seek(hid(env), env->get("offset").to_long(),
                       env->get("whence").to_string(), line, col);
            return Value();
          })),
      false);

  h.initialize(
      "tell",
      Value(FunctionValue({}, [hid, loc](std::shared_ptr<Environment> env) {
        int64_t line, col; loc(env, line, col);
        return Value(_file_tell(hid(env), line, col));
      }, "Long"sv)),
      false);

  // lines() — line iterator (newline stripped). dispose closes the handle
  // so a broken for-in still releases the fd.
  h.initialize(
      "lines",
      Value(FunctionValue({}, [hid, loc](std::shared_ptr<Environment> env) {
        int64_t line, col; loc(env, line, col);
        int64_t id = hid(env);
        // Capture the handle Value so the iterator keeps it alive — without
        // this the anonymous `File.open(p).lines()` handle would be GC'd
        // (drop → close) before the loop runs.
        Value self = env->get("self");
        Value iter = _make_iterator(
            [self, id, line, col](std::shared_ptr<Environment>) -> std::optional<Value> {
              std::string out;
              if (!_file_getline(id, out, line, col)) return _iter_step_done();
              return _iter_step_value(Value(std::move(out)));
            });
        _file_attach_dispose(iter, self, id);
        return iter;
      }, "Object"sv)),
      false);

  // chunks(n) — fixed-size byte chunks. Same dispose contract as lines().
  h.initialize(
      "chunks",
      Value(FunctionValue({{"n", false, "Long"sv}},
                          [hid, loc](std::shared_ptr<Environment> env) {
        int64_t line, col; loc(env, line, col);
        int64_t id = hid(env);
        int64_t n = env->get("n").to_long();
        Value self = env->get("self");
        Value iter = _make_iterator(
            [self, id, n, line, col](std::shared_ptr<Environment>) -> std::optional<Value> {
              auto chunk = _file_read_n(id, n, line, col);
              if (chunk.empty()) return _iter_step_done();
              return _iter_step_value(Value(std::move(chunk)));
            });
        _file_attach_dispose(iter, self, id);
        return iter;
      }, "Object"sv)),
      false);

  h.initialize(
      "close",
      Value(FunctionValue({}, [hid](std::shared_ptr<Environment> env) {
        _file_close(hid(env));
        return Value();
      })),
      false);

  // GC backstop: close a handle that was never explicitly closed.
  h.initialize(
      "drop",
      Value(FunctionValue({}, [hid](std::shared_ptr<Environment> env) {
        _file_close(hid(env));
        return Value();
      })),
      false);

  return Value(std::move(h));
}

inline Value make_file_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  ns.initialize(
      "open",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv},
           {"mode", false, ""sv, nullptr,
            std::make_shared<Value>(std::string("r"))}},
          [](std::shared_ptr<Environment> env) {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            int64_t id = _file_open(_fspath(env->get("path")),
                                    env->get("mode").to_string(), line, col);
            return make_file_handle(id);
          },
          "Object"sv)),
      false);

  // with(path, mode, fn) — scoped open: closes on every exit path, returns
  // the block's value. The native equivalent of open + defer { close }.
  ns.initialize(
      "with",
      Value(FunctionValue(
          {{"path", false, "String|Path"sv},
           {"mode", false, ""sv, nullptr,
            std::make_shared<Value>(std::string("r"))},
           {"fn", false, "Function"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            int64_t id = _file_open(_fspath(env->get("path")),
                                    env->get("mode").to_string(), line, col);
            Value handle = make_file_handle(id);
            const auto& fn = env->get("fn").to_function();
            auto call_env = _make_method_call_env(handle, line, col);
            if (!fn.params->empty()) {
              call_env->initialize((*fn.params)[0].name, handle, false);
            }
            try {
              Value result;
              // An explicit `return` in the block surfaces as ReturnValue;
              // unwrap it like every other call site so the value becomes
              // File.with's result (matches the JIT path).
              try {
                result = fn.eval(call_env);
              } catch (const ReturnValue& r) {
                result = r.value;
              }
              _file_close(id);
              return result;
            } catch (...) {
              _file_close(id);
              throw;
            }
          })),
      false);

  return Value(std::move(ns));
}

#if defined(CULEBRA_SQLITE_ENABLED)
// ===========================================================================
// SQLite — embedded SQL database over the value-neutral cursor core (sqlite.h).
// `SQLite.open(path)` returns a Database handle; db.execute/query/transaction
// run high-level SQL; db.prepare returns a reusable Statement handle. Handles
// are __nonsendable__ (the sqlite3*/sqlite3_stmt* live in a thread-local table)
// and close deterministically on scope exit via the `drop` backstop.
// ===========================================================================

[[noreturn]] inline void _sqlite_throw(const std::string& msg, int64_t line,
                                       int64_t col) {
  throw CulebraError("SQLiteError", std::format("SQLite: {}", msg), line, col);
}

// culebra value -> neutral BindVal. Long/Bool -> Integer, Float -> Float,
// String -> Text, nil -> Null; anything else is a TypeError.
inline culebra::sqlite::BindVal _sqlite_to_bind(const Value& v, int64_t line,
                                                int64_t col) {
  using CT = culebra::sqlite::ColType;
  culebra::sqlite::BindVal b;
  switch (v.type) {
    case Value::Nil:
      b.type = CT::Null;
      break;
    case Value::Bool:
      b.type = CT::Integer;
      b.i = v.to_bool() ? 1 : 0;
      break;
    case Value::Long:
      b.type = CT::Integer;
      b.i = v.to_long();
      break;
    case Value::Float:
      b.type = CT::Float;
      b.d = v.to_double_coerce();
      break;
    case Value::String:
    case Value::StringView:
      b.type = CT::Text;
      b.text = v.to_string_view();
      break;
    default:
      throw CulebraError(
          "TypeError",
          std::format("SQLite: cannot bind a {} value", v.type_name()), line,
          col);
  }
  return b;
}

// neutral Cell -> culebra value (runtime column-type mapping).
inline Value _sqlite_cell_to_value(const culebra::sqlite::Cell& c) {
  using CT = culebra::sqlite::ColType;
  switch (c.type) {
    case CT::Integer:
      return Value(static_cast<int64_t>(c.i));
    case CT::Float:
      return Value(c.d);
    case CT::Text:
    case CT::Blob:
      return Value(std::string(c.text));
    case CT::Null:
    default:
      return Value();
  }
}

// Bind `params` onto a prepared statement: an Array binds positionally (1-based
// `?`), an Object binds by name (`:key`/`@key`/`$key`), nil binds nothing.
inline void _sqlite_bind_params(int64_t stmt_id, const Value& params, int64_t line,
                                int64_t col) {
  std::string err;
  if (params.type == Value::Nil) return;
  if (params.type == Value::Array) {
    const auto& arr = *params.to_array().values;
    for (size_t i = 0; i < arr.size(); ++i) {
      auto b = _sqlite_to_bind(arr[i], line, col);
      if (!culebra::sqlite::bind(stmt_id, static_cast<int>(i) + 1, b, &err))
        _sqlite_throw(err, line, col);
    }
    return;
  }
  if (params.type == Value::Object) {
    const auto& obj = params.to_object();
    for (const Value& key : *obj.key_order) {
      if (key.type != Value::String && key.type != Value::StringView) {
        throw CulebraError("TypeError",
                           "SQLite: named parameter keys must be Strings", line,
                           col);
      }
      std::string name(key.to_string_view());
      int pi = culebra::sqlite::bind_index(stmt_id, ":" + name);
      if (pi == 0) pi = culebra::sqlite::bind_index(stmt_id, "@" + name);
      if (pi == 0) pi = culebra::sqlite::bind_index(stmt_id, "$" + name);
      if (pi == 0)
        _sqlite_throw(std::format("no such named parameter ':{}'", name), line,
                      col);
      auto b = _sqlite_to_bind(obj.get(key), line, col);
      if (!culebra::sqlite::bind(stmt_id, pi, b, &err))
        _sqlite_throw(err, line, col);
    }
    return;
  }
  throw CulebraError(
      "TypeError",
      std::format("SQLite: params must be an Array or Object, got {}",
                  params.type_name()),
      line, col);
}

// Drives a prepared statement to completion, collecting each row as an Object
// keyed by column name. Throws SQLiteError on a step failure.
inline Value _sqlite_collect_rows(int64_t stmt_id, int64_t line, int64_t col) {
  ArrayValue rows;
  int ncol = culebra::sqlite::column_count(stmt_id);
  std::vector<std::string> names(ncol);
  for (int i = 0; i < ncol; ++i) names[i] = culebra::sqlite::column_name(stmt_id, i);
  std::string err;
  for (;;) {
    int rc = culebra::sqlite::step(stmt_id, &err);
    if (rc < 0) _sqlite_throw(err, line, col);
    if (rc == 0) break;
    ObjectValue row;
    for (int i = 0; i < ncol; ++i)
      row.initialize(names[i], _sqlite_cell_to_value(culebra::sqlite::column(stmt_id, i)),
                     false);
    rows.values->push_back(Value(std::move(row)));
  }
  return Value(std::move(rows));
}

// Drains a prepared statement, ignoring any rows (for execute()).
inline void _sqlite_drain(int64_t stmt_id, int64_t line, int64_t col) {
  std::string err;
  for (;;) {
    int rc = culebra::sqlite::step(stmt_id, &err);
    if (rc < 0) _sqlite_throw(err, line, col);
    if (rc == 0) break;
  }
}

// execute(db, sql, params) -> rows affected. Prepares a transient statement.
inline Value _sqlite_execute(int64_t db_id, const std::string& sql,
                             const Value& params, int64_t line, int64_t col) {
  std::string err;
  int64_t st = culebra::sqlite::prepare(db_id, sql, &err);
  if (st < 0) _sqlite_throw(err, line, col);
  try {
    _sqlite_bind_params(st, params, line, col);
    _sqlite_drain(st, line, col);
  } catch (...) {
    culebra::sqlite::finalize(st);
    throw;
  }
  culebra::sqlite::finalize(st);
  return Value(static_cast<int64_t>(culebra::sqlite::changes(db_id)));
}

// query(db, sql, params) -> [Object]. Prepares a transient statement.
inline Value _sqlite_query(int64_t db_id, const std::string& sql,
                           const Value& params, int64_t line, int64_t col) {
  std::string err;
  int64_t st = culebra::sqlite::prepare(db_id, sql, &err);
  if (st < 0) _sqlite_throw(err, line, col);
  Value rows;
  try {
    _sqlite_bind_params(st, params, line, col);
    rows = _sqlite_collect_rows(st, line, col);
  } catch (...) {
    culebra::sqlite::finalize(st);
    throw;
  }
  culebra::sqlite::finalize(st);
  return rows;
}

// Build a Statement handle: `_id` into the stmt table, `__parent__` holding the
// owning Database handle so the connection outlives the statement (the GC keeps
// the parent reachable). `finalize`/`drop` release the statement.
inline Value make_sqlite_stmt_handle(int64_t stmt_id, const Value& db_handle) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("_id", Value(static_cast<int64_t>(stmt_id)), false);
  h.initialize("__nonsendable__", Value(true), false);
  // Keep the owning Database alive while this Statement exists.
  h.initialize("__parent__", db_handle, false);

  auto sid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("self").to_object().get("_id").to_long();
  };
  auto dbid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("self").to_object().get("__parent__").to_object().get("_id").to_long();
  };
  auto loc = [](const std::shared_ptr<Environment>& env, int64_t& line,
                int64_t& col) {
    line = env->get("__LINE__").to_long();
    col = env->get("__COLUMN__").to_long();
  };
  auto params_param =
      FunctionValue::Parameter{"params", false, ""sv, nullptr, kw_default_nil()};

  // run(params=nil) -> rows affected (INSERT/UPDATE/DELETE).
  h.initialize(
      "run",
      Value(FunctionValue(
          {params_param},
          [sid, dbid, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            int64_t st = sid(env);
            culebra::sqlite::reset(st);
            _sqlite_bind_params(st, env->get("params"), line, col);
            _sqlite_drain(st, line, col);
            return Value(static_cast<int64_t>(culebra::sqlite::changes(dbid(env))));
          },
          "Long"sv)),
      false);

  // query(params=nil) -> [Object].
  h.initialize(
      "query",
      Value(FunctionValue(
          {params_param},
          [sid, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            int64_t st = sid(env);
            culebra::sqlite::reset(st);
            _sqlite_bind_params(st, env->get("params"), line, col);
            return _sqlite_collect_rows(st, line, col);
          },
          "Array"sv)),
      false);

  h.initialize(
      "finalize",
      Value(FunctionValue({}, [sid](std::shared_ptr<Environment> env) {
        culebra::sqlite::finalize(sid(env));
        return Value();
      })),
      false);

  // GC backstop: finalize a statement that was never explicitly finalized.
  h.initialize(
      "drop",
      Value(FunctionValue({}, [sid](std::shared_ptr<Environment> env) {
        culebra::sqlite::finalize(sid(env));
        return Value();
      })),
      false);

  return Value(std::move(h));
}

// Build a Database handle: `_id` into the connection table plus the
// execute/query/transaction/prepare/close method set. `close`/`drop` both close
// the connection (idempotent; sqlite3_close_v2 defers if statements are live).
inline Value make_sqlite_db_handle(int64_t db_id) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("_id", Value(static_cast<int64_t>(db_id)), false);
  h.initialize("__nonsendable__", Value(true), false);

  auto did = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("self").to_object().get("_id").to_long();
  };
  auto loc = [](const std::shared_ptr<Environment>& env, int64_t& line,
                int64_t& col) {
    line = env->get("__LINE__").to_long();
    col = env->get("__COLUMN__").to_long();
  };
  auto sql_param = FunctionValue::Parameter{"sql", false, "String"sv};
  auto params_param =
      FunctionValue::Parameter{"params", false, ""sv, nullptr, kw_default_nil()};

  // execute(sql, params=nil) -> rows affected.
  h.initialize(
      "execute",
      Value(FunctionValue(
          {sql_param, params_param},
          [did, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            return _sqlite_execute(did(env), env->get("sql").to_string(),
                                   env->get("params"), line, col);
          },
          "Long"sv)),
      false);

  // query(sql, params=nil) -> [Object].
  h.initialize(
      "query",
      Value(FunctionValue(
          {sql_param, params_param},
          [did, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            return _sqlite_query(did(env), env->get("sql").to_string(),
                                 env->get("params"), line, col);
          },
          "Array"sv)),
      false);

  // prepare(sql) -> Statement (reusable). Holds this Database as its parent.
  h.initialize(
      "prepare",
      Value(FunctionValue(
          {sql_param},
          [did, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            std::string err;
            int64_t st =
                culebra::sqlite::prepare(did(env), env->get("sql").to_string(), &err);
            if (st < 0) _sqlite_throw(err, line, col);
            return make_sqlite_stmt_handle(st, env->get("self"));
          },
          "Object"sv)),
      false);

  // transaction(fn) -> fn's value. BEGIN, run fn, COMMIT; ROLLBACK + rethrow on
  // any throw. Does not nest (SQLite has no nested BEGIN; use SAVEPOINT).
  h.initialize(
      "transaction",
      Value(FunctionValue(
          {{"fn", false, "Function"sv}},
          [did, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            int64_t db = did(env);
            Value fn = env->get("fn");
            _sqlite_execute(db, "BEGIN", Value(), line, col);
            auto interp = std::make_shared<Interpreter>();
            try {
              Value result = interp->call_closure(fn, env, {});
              _sqlite_execute(db, "COMMIT", Value(), line, col);
              return result;
            } catch (...) {
              _sqlite_execute(db, "ROLLBACK", Value(), line, col);
              throw;
            }
          })),
      false);

  h.initialize(
      "close",
      Value(FunctionValue({}, [did](std::shared_ptr<Environment> env) {
        culebra::sqlite::close_db(did(env));
        return Value();
      })),
      false);

  // GC backstop: close a connection that was never explicitly closed.
  h.initialize(
      "drop",
      Value(FunctionValue({}, [did](std::shared_ptr<Environment> env) {
        culebra::sqlite::close_db(did(env));
        return Value();
      })),
      false);

  return Value(std::move(h));
}

inline Value make_sqlite_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  // open(path) -> Database. ":memory:" for an in-memory database.
  ns.initialize(
      "open",
      Value(FunctionValue(
          {{"path", false, "String"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            std::string err;
            int64_t id =
                culebra::sqlite::open_db(env->get("path").to_string(), &err);
            if (id < 0) _sqlite_throw(err, line, col);
            return make_sqlite_db_handle(id);
          },
          "Object"sv)),
      false);

  // version() -> the linked SQLite library version (e.g. "3.53.2").
  ns.initialize(
      "version",
      Value(FunctionValue({}, [](std::shared_ptr<Environment>) {
        return Value(std::string(culebra::sqlite::libversion()));
      }, "String"sv)),
      false);

  return Value(std::move(ns));
}
#endif  // CULEBRA_SQLITE_ENABLED

// Net — TCP/UDP sockets over the value-neutral socket core (net.h). Three
// handle shapes (Socket / Listener / UdpSocket) each carry `_id` into the core's
// thread-local table; `close`/`drop` release it (idempotent). Framing lives in
// the core, so these adapters only translate values and errors.

[[noreturn]] inline void _net_throw(const char* ctx, const std::string& msg,
                                    int64_t line, int64_t col) {
  throw CulebraError("NetError", std::format("{}: {}", ctx, msg), line, col);
}

// Map a failed IoStatus to NetError. Eof is never an error here — each reader
// gives it its own meaning ("" / nil / a short-read error).
inline void _net_check(culebra::net::IoStatus st, const char* ctx,
                       const std::string& err, int64_t line, int64_t col) {
  if (st == culebra::net::IoStatus::Timeout ||
      st == culebra::net::IoStatus::Error) {
    _net_throw(ctx, culebra::net::status_message(st, err), line, col);
  }
}

inline Value _net_addr_object(const std::string& host, int port) {
  ObjectValue o;
  o.initialize("host", Value(std::string(host)), false);
  o.initialize("port", Value(static_cast<int64_t>(port)), false);
  return Value(std::move(o));
}

// The `_id` + set_timeout/is_open/close/drop set every Net handle shares.
inline void _net_add_common(ObjectValue& h, int64_t id) {
  using namespace std::literals;
  h.initialize("_id", Value(static_cast<int64_t>(id)), false);
  // A native handle (the fd lives in a thread-local table) is not Sendable —
  // reject it at the serialize boundary the same way the JIT handle does.
  h.initialize("__nonsendable__", Value(true), false);

  auto hid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("self").to_object().get("_id").to_long();
  };

  // set_timeout(ms) — 0 waits forever (still interruptible by Ctrl+C).
  h.initialize(
      "set_timeout",
      Value(FunctionValue({{"ms", false, "Long"sv}},
                          [hid](std::shared_ptr<Environment> env) {
                            int64_t line = env->get("__LINE__").to_long();
                            int64_t col = env->get("__COLUMN__").to_long();
                            std::string err;
                            if (!culebra::net::set_timeout(
                                    hid(env), env->get("ms").to_long(), &err))
                              _net_throw("Net.set_timeout", err, line, col);
                            return Value();
                          })),
      false);

  h.initialize(
      "is_open",
      Value(FunctionValue({},
                          [hid](std::shared_ptr<Environment> env) {
                            return Value(culebra::net::is_open(hid(env)));
                          },
                          "Bool"sv)),
      false);

  h.initialize(
      "close",
      Value(FunctionValue({}, [hid](std::shared_ptr<Environment> env) {
        culebra::net::close_handle(hid(env));
        return Value();
      })),
      false);

  // GC backstop: close a socket that was never explicitly closed.
  h.initialize(
      "drop",
      Value(FunctionValue({}, [hid](std::shared_ptr<Environment> env) {
        culebra::net::close_handle(hid(env));
        return Value();
      })),
      false);
}

// Build a Socket handle (a connected TCP stream): the File-shaped reader/writer
// set (read / read_line / read_exact / lines / write) plus socket specifics.
inline Value make_net_socket_handle(int64_t id) {
  using namespace std::literals;
  ObjectValue h;
  _net_add_common(h, id);

  auto hid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("self").to_object().get("_id").to_long();
  };
  auto loc = [](const std::shared_ptr<Environment>& env, int64_t& line,
                int64_t& col) {
    line = env->get("__LINE__").to_long();
    col = env->get("__COLUMN__").to_long();
  };

  // read(n=nil) — nil reads until the peer closes; a Long reads up to n bytes
  // (a short read is normal on a socket). "" at EOF, mirroring File.read.
  h.initialize(
      "read",
      Value(FunctionValue(
          {{"n", false, ""sv, nullptr, kw_default_nil()}},
          [hid, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            const auto& n = env->get("n");
            std::string out, err;
            culebra::net::IoStatus st;
            if (n.type == Value::Nil) {
              st = culebra::net::read_all(hid(env), out, &err);
            } else {
              int64_t want = n.to_long();
              st = culebra::net::read(hid(env),
                                      static_cast<size_t>(want < 0 ? 0 : want),
                                      out, &err);
            }
            _net_check(st, "Net.read", err, line, col);
            return Value(std::move(out));  // Eof -> ""
          },
          "String"sv)),
      false);

  // read_line() — one line, terminator stripped; nil once the stream ends.
  h.initialize(
      "read_line",
      Value(FunctionValue(
          {},
          [hid, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            std::string out, err;
            auto st = culebra::net::read_line(hid(env), out, &err);
            if (st == culebra::net::IoStatus::Eof) return Value();
            _net_check(st, "Net.read_line", err, line, col);
            return Value(std::move(out));
          },
          "String?"sv)),
      false);

  // read_exact(n) — exactly n bytes; a peer that closes early is an error, not
  // a short read (the point of asking for an exact frame).
  h.initialize(
      "read_exact",
      Value(FunctionValue(
          {{"n", false, "Long"sv}},
          [hid, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            int64_t want = env->get("n").to_long();
            if (want < 0) want = 0;
            std::string out, err;
            auto st = culebra::net::read_exact(
                hid(env), static_cast<size_t>(want), out, &err);
            if (st == culebra::net::IoStatus::Eof) {
              _net_throw("Net.read_exact",
                         std::format("unexpected EOF ({} of {} bytes)",
                                     out.size(), want),
                         line, col);
            }
            _net_check(st, "Net.read_exact", err, line, col);
            return Value(std::move(out));
          },
          "String"sv)),
      false);

  // lines() — line iterator, ending when the peer closes. Unlike File.lines it
  // does not close on exit: a socket is usually still written to afterwards.
  h.initialize(
      "lines",
      Value(FunctionValue(
          {},
          [hid, loc](std::shared_ptr<Environment> env) {
            int64_t line, col; loc(env, line, col);
            int64_t id = hid(env);
            // Capture the handle so the iterator keeps it alive — otherwise an
            // anonymous `Net.connect(...).lines()` would be GC'd (drop → close)
            // before the loop runs.
            Value self = env->get("self");
            return _make_iterator(
                [self, id, line, col](
                    std::shared_ptr<Environment>) -> std::optional<Value> {
                  std::string out, err;
                  auto st = culebra::net::read_line(id, out, &err);
                  if (st == culebra::net::IoStatus::Eof)
                    return _iter_step_done();
                  _net_check(st, "Net.lines", err, line, col);
                  return _iter_step_value(Value(std::move(out)));
                });
          },
          "Object"sv)),
      false);

  h.initialize(
      "write",
      Value(FunctionValue(
          {{"data", false, "String"sv}},
          [hid, loc](std::shared_ptr<Environment> env) {
            int64_t line, col; loc(env, line, col);
            const std::string& data = env->get("data").to_string();
            std::string err;
            auto st = culebra::net::write_all(hid(env), data.data(),
                                              data.size(), &err);
            _net_check(st, "Net.write", err, line, col);
            return Value();
          })),
      false);

  // shutdown_write() — half-close: signal EOF to the peer while still reading
  // its reply (the request/response idiom of line protocols).
  h.initialize(
      "shutdown_write",
      Value(FunctionValue({}, [hid, loc](std::shared_ptr<Environment> env) {
        int64_t line, col; loc(env, line, col);
        std::string err;
        if (!culebra::net::shutdown_write(hid(env), &err))
          _net_throw("Net.shutdown_write", err, line, col);
        return Value();
      })),
      false);

  h.initialize(
      "set_nodelay",
      Value(FunctionValue(
          {{"on", false, "Bool"sv, nullptr,
            std::make_shared<Value>(Value(true))}},
          [hid, loc](std::shared_ptr<Environment> env) {
            int64_t line, col; loc(env, line, col);
            std::string err;
            if (!culebra::net::set_nodelay(hid(env), env->get("on").to_bool(),
                                           &err))
              _net_throw("Net.set_nodelay", err, line, col);
            return Value();
          })),
      false);

  h.initialize(
      "local_addr",
      Value(FunctionValue(
          {},
          [hid, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            std::string host, err;
            int port = 0;
            if (!culebra::net::local_addr(hid(env), host, port, &err))
              _net_throw("Net.local_addr", err, line, col);
            return _net_addr_object(host, port);
          },
          "Object"sv)),
      false);

  h.initialize(
      "peer_addr",
      Value(FunctionValue(
          {},
          [hid, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            std::string host, err;
            int port = 0;
            if (!culebra::net::peer_addr(hid(env), host, port, &err))
              _net_throw("Net.peer_addr", err, line, col);
            return _net_addr_object(host, port);
          },
          "Object"sv)),
      false);

  return Value(std::move(h));
}

// Defined after isolate.h is included (it needs sendable::serialize, since the
// handler runs on a worker pool); forward-declared here so the listener handle
// method can call it. Serves until interrupted; throws on a Sendable error.
inline Value _net_serve_impl(int64_t id, const Value& handler, int64_t workers,
                             int64_t line, int64_t col);

// Build a Listener handle: `port`/`host` of the bound address (so a port-0 bind
// can report the ephemeral port it got) plus accept, and `iter` so
// `for conn in listener` accepts in a loop.
inline Value make_net_listener_handle(int64_t id, const std::string& host,
                                      int port) {
  using namespace std::literals;
  ObjectValue h;
  _net_add_common(h, id);
  h.initialize("host", Value(std::string(host)), false);
  h.initialize("port", Value(static_cast<int64_t>(port)), false);

  auto hid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("self").to_object().get("_id").to_long();
  };
  auto loc = [](const std::shared_ptr<Environment>& env, int64_t& line,
                int64_t& col) {
    line = env->get("__LINE__").to_long();
    col = env->get("__COLUMN__").to_long();
  };

  h.initialize(
      "accept",
      Value(FunctionValue(
          {},
          [hid, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            int64_t cid = -1;
            std::string err;
            auto st = culebra::net::accept(hid(env), &cid, &err);
            _net_check(st, "Net.accept", err, line, col);
            return make_net_socket_handle(cid);
          },
          "Object"sv)),
      false);

  // serve(handler, workers=0) — accept on this thread, run `handler(conn)` on a
  // worker pool. Blocks until interrupted. The handler must be Sendable (each
  // worker rebuilds it on its own heap), so this is the concurrent counterpart
  // to the sequential `accept` / `for conn in listener`.
  h.initialize(
      "serve",
      Value(FunctionValue(
          {{"handler", false, "Function"sv},
           {"workers", false, "Long"sv, nullptr,
            std::make_shared<Value>(Value(static_cast<int64_t>(0)))}},
          [hid, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            return _net_serve_impl(hid(env), env->get("handler"),
                                   env->get("workers").to_long(), line, col);
          })),
      false);

  // iter() — the Iterable trait: an endless accept loop. It never ends on its
  // own (a listener has no EOF), so the body breaks out; closing the listener
  // raises rather than silently ending the loop.
  h.initialize(
      "iter",
      Value(FunctionValue(
          {},
          [hid, loc](std::shared_ptr<Environment> env) {
            int64_t line, col; loc(env, line, col);
            int64_t id = hid(env);
            Value self = env->get("self");
            return _make_iterator(
                [self, id, line, col](
                    std::shared_ptr<Environment>) -> std::optional<Value> {
                  int64_t cid = -1;
                  std::string err;
                  auto st = culebra::net::accept(id, &cid, &err);
                  _net_check(st, "Net.accept", err, line, col);
                  return _iter_step_value(make_net_socket_handle(cid));
                });
          },
          "Object"sv)),
      false);

  return Value(std::move(h));
}

// Build a UdpSocket handle: bound `host`/`port` plus send_to / recv_from.
inline Value make_net_udp_handle(int64_t id, const std::string& host, int port) {
  using namespace std::literals;
  ObjectValue h;
  _net_add_common(h, id);
  h.initialize("host", Value(std::string(host)), false);
  h.initialize("port", Value(static_cast<int64_t>(port)), false);

  auto hid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("self").to_object().get("_id").to_long();
  };
  auto loc = [](const std::shared_ptr<Environment>& env, int64_t& line,
                int64_t& col) {
    line = env->get("__LINE__").to_long();
    col = env->get("__COLUMN__").to_long();
  };

  h.initialize(
      "send_to",
      Value(FunctionValue(
          {{"data", false, "String"sv},
           {"host", false, "String"sv},
           {"port", false, "Long"sv}},
          [hid, loc](std::shared_ptr<Environment> env) {
            int64_t line, col; loc(env, line, col);
            const std::string& data = env->get("data").to_string();
            std::string err;
            if (!culebra::net::udp_send_to(
                    hid(env), data.data(), data.size(),
                    env->get("host").to_string(),
                    static_cast<int>(env->get("port").to_long()), &err))
              _net_throw("Net.send_to", err, line, col);
            return Value();
          })),
      false);

  // recv_from(max=65536) -> {data, host, port}. An oversized datagram is
  // truncated to `max`, as UDP dictates.
  h.initialize(
      "recv_from",
      Value(FunctionValue(
          {{"max", false, "Long"sv, nullptr,
            std::make_shared<Value>(Value(static_cast<int64_t>(65536)))}},
          [hid, loc](std::shared_ptr<Environment> env) -> Value {
            int64_t line, col; loc(env, line, col);
            int64_t max = env->get("max").to_long();
            if (max < 0) max = 0;
            std::string data, host, err;
            int port = 0;
            auto st = culebra::net::udp_recv_from(
                hid(env), static_cast<size_t>(max), data, host, port, &err);
            _net_check(st, "Net.recv_from", err, line, col);
            ObjectValue o;
            o.initialize("data", Value(std::move(data)), false);
            o.initialize("host", Value(std::move(host)), false);
            o.initialize("port", Value(static_cast<int64_t>(port)), false);
            return Value(std::move(o));
          },
          "Object"sv)),
      false);

  h.initialize(
      "set_broadcast",
      Value(FunctionValue(
          {{"on", false, "Bool"sv, nullptr,
            std::make_shared<Value>(Value(true))}},
          [hid, loc](std::shared_ptr<Environment> env) {
            int64_t line, col; loc(env, line, col);
            std::string err;
            if (!culebra::net::set_broadcast(hid(env), env->get("on").to_bool(),
                                             &err))
              _net_throw("Net.set_broadcast", err, line, col);
            return Value();
          })),
      false);

  return Value(std::move(h));
}

// Read back the address a fresh listener / UDP socket actually bound, so the
// handle can report the ephemeral port chosen for a port-0 bind.
inline void _net_bound_addr(int64_t id, const char* ctx, std::string& host,
                            int& port, int64_t line, int64_t col) {
  std::string err;
  if (!culebra::net::local_addr(id, host, port, &err)) {
    culebra::net::close_handle(id);
    _net_throw(ctx, err, line, col);
  }
}

inline Value make_net_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  auto timeout_param = FunctionValue::Parameter{
      "timeout", false, "Long"sv, nullptr,
      std::make_shared<Value>(Value(static_cast<int64_t>(0)))};

  // connect(host, port, timeout=0) -> Socket. `timeout` (ms, 0 = none) bounds
  // the connect and becomes the socket's read/write timeout.
  ns.initialize(
      "connect",
      Value(FunctionValue(
          {{"host", false, "String"sv}, {"port", false, "Long"sv},
           timeout_param},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            std::string err;
            int64_t id = culebra::net::connect(
                env->get("host").to_string(),
                static_cast<int>(env->get("port").to_long()),
                env->get("timeout").to_long(), &err);
            if (id < 0) _net_throw("Net.connect", err, line, col);
            return make_net_socket_handle(id);
          },
          "Object"sv)),
      false);

  // listen(port, host="0.0.0.0", backlog=0) -> Listener. Port 0 binds an
  // ephemeral port, readable as `listener.port`.
  ns.initialize(
      "listen",
      Value(FunctionValue(
          {{"port", false, "Long"sv},
           {"host", false, "String"sv, nullptr,
            std::make_shared<Value>(std::string("0.0.0.0"))},
           {"backlog", false, "Long"sv, nullptr,
            std::make_shared<Value>(Value(static_cast<int64_t>(0)))}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            std::string err;
            int64_t id = culebra::net::listen(
                env->get("host").to_string(),
                static_cast<int>(env->get("port").to_long()),
                static_cast<int>(env->get("backlog").to_long()), &err);
            if (id < 0) _net_throw("Net.listen", err, line, col);
            std::string host;
            int port = 0;
            _net_bound_addr(id, "Net.listen", host, port, line, col);
            return make_net_listener_handle(id, host, port);
          },
          "Object"sv)),
      false);

  // udp(port=0, host="0.0.0.0") -> UdpSocket.
  ns.initialize(
      "udp",
      Value(FunctionValue(
          {{"port", false, "Long"sv, nullptr,
            std::make_shared<Value>(Value(static_cast<int64_t>(0)))},
           {"host", false, "String"sv, nullptr,
            std::make_shared<Value>(std::string("0.0.0.0"))}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            std::string err;
            int64_t id = culebra::net::udp_open(
                env->get("host").to_string(),
                static_cast<int>(env->get("port").to_long()), &err);
            if (id < 0) _net_throw("Net.udp", err, line, col);
            std::string host;
            int port = 0;
            _net_bound_addr(id, "Net.udp", host, port, line, col);
            return make_net_udp_handle(id, host, port);
          },
          "Object"sv)),
      false);

  // resolve(host) -> [String]: the numeric addresses `host` resolves to.
  ns.initialize(
      "resolve",
      Value(FunctionValue(
          {{"host", false, "String"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            std::vector<std::string> addrs;
            std::string err;
            if (!culebra::net::resolve(env->get("host").to_string(), addrs,
                                       &err))
              _net_throw("Net.resolve", err, line, col);
            ArrayValue av;
            for (auto& a : addrs) av.values->emplace_back(Value(std::move(a)));
            return Value(std::move(av));
          },
          "Array"sv)),
      false);

  return Value(std::move(ns));
}

// Build the Proc.spawn live handle: data fields `_pid/_out/_err/_done/_result`
// plus `wait`/`poll`/`kill`/`drop` methods. The result is cached on first
// wait/poll so the methods are idempotent; `drop` (called on GC) best-effort
// reaps a child that was never waited on.
inline Value make_proc_handle(int64_t pid, int out_fd, int err_fd) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("_pid", Value(pid), true);
  h.initialize("_out", Value(static_cast<int64_t>(out_fd)), true);
  h.initialize("_err", Value(static_cast<int64_t>(err_fd)), true);
  h.initialize("_done", Value(false), true);
  h.initialize("_result", Value(), true);
  // A native handle (pid + pipe fds are process-local) is not Sendable — reject
  // it at the serialize boundary the same way the JIT handle does.
  h.initialize("__nonsendable__", Value(true), false);

  h.initialize("wait",
      Value(FunctionValue({}, [](std::shared_ptr<Environment> env) -> Value {
        const Value& self = env->get("self");
        const auto& o = self.to_object();
        if (o.get("_done").to_bool()) return o.get("_result");
        int out_fd = static_cast<int>(o.get("_out").to_long());
        int err_fd = static_cast<int>(o.get("_err").to_long());
        auto pr = culebra::proc::wait_handle(o.get("_pid").to_long(), out_fd,
                                             err_fd);
        Value res = proc_result_to_value(std::move(pr));
        _proc_handle_set(self, "_done", Value(true));
        _proc_handle_set(self, "_result", res);
        return res;
      }, "Object"sv)), false);

  h.initialize("poll",
      Value(FunctionValue({}, [](std::shared_ptr<Environment> env) -> Value {
        const Value& self = env->get("self");
        const auto& o = self.to_object();
        if (o.get("_done").to_bool()) return o.get("_result");
        int status = 0;
        if (!culebra::proc::try_reap(o.get("_pid").to_long(), status)) {
          return Value();  // still running
        }
        int out_fd = static_cast<int>(o.get("_out").to_long());
        int err_fd = static_cast<int>(o.get("_err").to_long());
        auto pr = culebra::proc::drain_reaped(status, out_fd, err_fd);
        Value res = proc_result_to_value(std::move(pr));
        _proc_handle_set(self, "_done", Value(true));
        _proc_handle_set(self, "_result", res);
        return res;
      }, ""sv)), false);  // returns Object (exited) or nil (still running)

  static const auto kill_sig_default =
      std::make_shared<Value>(Value(static_cast<int64_t>(15)));
  h.initialize("kill",
      Value(FunctionValue({{"sig", false, "Long"sv, nullptr, kill_sig_default}},
          [](std::shared_ptr<Environment> env) -> Value {
            const auto& o = env->get("self").to_object();
            if (!o.get("_done").to_bool()) {
              culebra::proc::kill_pid(
                  o.get("_pid").to_long(),
                  static_cast<int>(env->get("sig").to_long()));
            }
            return Value();
          }, "Nil"sv)), false);

  h.initialize("drop",
      Value(FunctionValue({}, [](std::shared_ptr<Environment> env) -> Value {
        const Value& self = env->get("self");
        const auto& o = self.to_object();
        if (o.get("_done").to_bool()) return Value();
        int out_fd = static_cast<int>(o.get("_out").to_long());
        int err_fd = static_cast<int>(o.get("_err").to_long());
        int64_t pid = o.get("_pid").to_long();
        culebra::proc::kill_pid(pid, SIGKILL);
        culebra::proc::wait_handle(pid, out_fd, err_fd);  // drains + reaps
        _proc_handle_set(self, "_done", Value(true));
        return Value();
      }, "Nil"sv)), false);

  return Value(std::move(h));
}

// Defined later in this header; forward-declared here (before the HTTP block and
// with defaults) so both the Http helpers' `json:` kwarg / `r.json()` method AND
// the debug adapter (dap.h, an HTTP-independent consumer) see the defaulted
// signature regardless of whether the Http namespace is compiled in.
inline std::string json_stringify(const Value& v, int indent, bool sort_keys);
inline Value json_parse(std::string_view s, std::string_view number_mode = "auto",
                        bool jsonc = false);

#if defined(CULEBRA_HTTP_ENABLED)
// Convert the optional `headers` kwarg (nil or an Object of String values)
// into the header list the http core wants. `ctx` tags type errors.
inline culebra::http::HeaderList http_parse_headers(const Value& hv,
                                                    const char* ctx, int64_t line,
                                                    int64_t col) {
  culebra::http::HeaderList headers;
  if (hv.type == Value::Nil) return headers;
  if (hv.type != Value::Object) {
    throw CulebraError("TypeError",
        std::format("{}: headers must be an Object of String", ctx), line, col);
  }
  for (const auto& [k, sym] : *hv.to_object().properties) {
    if (sym.val.type != Value::String && sym.val.type != Value::StringView) {
      throw CulebraError("TypeError",
          std::format("{}: header values must be String", ctx), line, col);
    }
    headers.emplace_back(std::string(k),
                         std::string(sym.val.to_string_view()));
  }
  return headers;
}

// Convert the optional `params` kwarg (nil or an Object of String values) into
// query name/value pairs (percent-encoded by the http core). `ctx` tags errors.
inline culebra::http::HeaderList http_parse_params(const Value& pv,
                                                   const char* ctx, int64_t line,
                                                   int64_t col) {
  culebra::http::HeaderList params;
  if (pv.type == Value::Nil) return params;
  if (pv.type != Value::Object) {
    throw CulebraError("TypeError",
        std::format("{}: params must be an Object of String", ctx), line, col);
  }
  for (const auto& [k, sym] : *pv.to_object().properties) {
    if (sym.val.type != Value::String && sym.val.type != Value::StringView) {
      throw CulebraError("TypeError",
          std::format("{}: param values must be String", ctx), line, col);
    }
    params.emplace_back(std::string(k),
                        std::string(sym.val.to_string_view()));
  }
  return params;
}

// Convert the `form` kwarg (an Object of String values) into name/value pairs
// for an application/x-www-form-urlencoded body. `ctx` tags errors.
inline culebra::http::HeaderList http_parse_form(const Value& fv,
                                                 const char* ctx, int64_t line,
                                                 int64_t col) {
  culebra::http::HeaderList form;
  if (fv.type != Value::Object) {
    throw CulebraError("TypeError",
        std::format("{}: form must be an Object of String", ctx), line, col);
  }
  for (const auto& [k, sym] : *fv.to_object().properties) {
    if (sym.val.type != Value::String && sym.val.type != Value::StringView) {
      throw CulebraError("TypeError",
          std::format("{}: form values must be String", ctx), line, col);
    }
    form.emplace_back(std::string(k), std::string(sym.val.to_string_view()));
  }
  return form;
}

// Wrap an HttpResult as the response Object `{status, ok, body, headers}`.
// `ok` is true iff the status is 2xx (a 4xx/5xx is a completed round-trip,
// not a transport error). Transport failures never reach here — the caller
// throws HttpError on `!result.ok`.
inline Value http_result_to_value(culebra::http::HttpResult&& r) {
  using namespace std::literals;
  ObjectValue obj;
  obj.initialize("status", Value(r.status), false);
  obj.initialize("ok", Value(r.status >= 200 && r.status < 300), false);
  obj.initialize("reason", Value(std::move(r.reason)), false);
  obj.initialize("body", Value(std::move(r.body)), false);
  ObjectValue headers;
  for (auto& [k, v] : r.headers) {
    headers.initialize(k, Value(std::move(v)), false);
  }
  obj.initialize("headers", Value(std::move(headers)), false);
  // `r.json()` — parse the body as JSON (convenience for `JSON.parse(r.body)`).
  obj.initialize(
      "json",
      Value(FunctionValue({}, [](std::shared_ptr<Environment> env) {
        return json_parse(env->get("self").to_object().get("body")
                              .to_string_view(), "auto"sv);
      }, "Any"sv)),
      false);
  return Value(std::move(obj));
}

// Read the standard tail kwargs (`headers`, `timeout`, `follow_redirects`) off
// the call environment into `req`. `body`/`content_type`/`method`/`url` are
// set by the individual builders.
inline void http_fill_common(const std::shared_ptr<Environment>& env,
                             culebra::http::HttpRequest& req, const char* ctx,
                             int64_t line, int64_t col) {
  req.headers = http_parse_headers(env->get("headers"), ctx, line, col);
  req.params = http_parse_params(env->get("params"), ctx, line, col);
  // nil means "unset" → use the default (matching every other optional kwarg
  // here and the JIT, which can't tell an explicit `timeout: nil` from a
  // positionally-absent slot). A present non-nil still goes through the strict
  // to_long()/to_bool() coercion.
  const auto& tv = env->get("timeout");
  int64_t timeout = tv.type == Value::Nil ? 0 : tv.to_long();
  req.timeout_sec = timeout > 0 ? timeout : 0;
  const auto& fv = env->get("follow_redirects");
  req.follow_redirects = fv.type == Value::Nil ? true : fv.to_bool();
}

// Backing state for an `into:` response sink, owned by the call for the whole
// request: the output file (String into), the callback's interpreter, and any
// exception the callback raised (rethrown after the request returns).
struct HttpIntoState {
  std::ofstream ofs;
  std::shared_ptr<Interpreter> cb_interp;
  std::exception_ptr eptr;
};

// Configure `req.body_sink` from the `into` kwarg, streaming the response body
// instead of buffering it: nil → buffer (no streaming); String → write the
// body to that file path; Function → call it with each chunk. Else → TypeError.
inline void http_setup_into(const Value& into,
                            const std::shared_ptr<Environment>& env,
                            culebra::http::HttpRequest& req, HttpIntoState& st,
                            const char* ctx, int64_t line, int64_t col) {
  if (into.type == Value::Nil) return;
  if (into.type == Value::String || into.type == Value::StringView) {
    std::string path(into.to_string_view());
    st.ofs.open(path, std::ios::binary);
    if (!st.ofs) {
      throw CulebraError(
          "IOError",
          std::format("{}: cannot open '{}' for writing", ctx, path), line,
          col);
    }
    req.body_sink = [&st](const char* d, size_t n) {
      st.ofs.write(d, static_cast<std::streamsize>(n));
      return static_cast<bool>(st.ofs);
    };
  } else if (into.type == Value::Function) {
    st.cb_interp = std::make_shared<Interpreter>();
    Value cb = into;
    req.body_sink = [&st, cb, env](const char* d, size_t n) -> bool {
      try {
        std::vector<Value> args;
        args.emplace_back(std::string(d, n));
        st.cb_interp->call_closure(cb, env, std::move(args));
        return true;
      } catch (...) {
        st.eptr = std::current_exception();  // abort; rethrown after send
        return false;
      }
    };
  } else {
    throw CulebraError(
        "TypeError",
        std::format("{}: into must be a String path or a Function", ctx), line,
        col);
  }
}

// Run a request (whose sink may have been set by http_setup_into): rethrow a
// callback exception first, then map a transport failure to HttpError, else
// return the response Object ({status, ok, body, headers}; body empty when
// streamed via `into`).
inline Value http_run_into(culebra::http::HttpRequest& req, HttpIntoState& st,
                           const char* ctx, int64_t line, int64_t col) {
  auto r = culebra::http::http_request(req);
  if (st.eptr) std::rethrow_exception(st.eptr);
  if (!r.ok) {
    throw CulebraError("HttpError", std::format("{}: {}", ctx, r.error), line,
                       col);
  }
  return http_result_to_value(std::move(r));
}

// As http_run_into, but against a persistent Http.client (id) — reuses its
// connection and layers its default headers under the request's.
inline Value http_run_client_into(int64_t id, culebra::http::HttpRequest& req,
                                  HttpIntoState& st, const char* ctx, int64_t line,
                                  int64_t col) {
  auto r = culebra::http::http_client_request(id, req);
  if (st.eptr) std::rethrow_exception(st.eptr);
  if (!r.ok) {
    throw CulebraError("HttpError", std::format("{}: {}", ctx, r.error), line,
                       col);
  }
  return http_result_to_value(std::move(r));
}

// Parse the `files` kwarg into req.multipart for a multipart/form-data upload.
// Each Object entry is a part; its value is one of:
//   - String                                  → text field {name: value}
//   - {content: String, filename?, content_type?}  → in-memory file part
//   - {path: String, filename?, content_type?}      → streamed from disk
//   - {stream: Function, filename?, content_type?}  → streamed from a producer
//   - Array of any of the above               → repeated parts under one name
// A path/stream part is streamed chunked so a big file or slow output never has
// to live in memory at once. `ctx` tags type errors.
inline void http_setup_multipart(const Value& filesv,
                                  const std::shared_ptr<Environment>& env,
                                  culebra::http::HttpRequest& req,
                                  HttpIntoState& st, const char* ctx, int64_t line,
                                  int64_t col) {
  if (filesv.type != Value::Object) {
    throw CulebraError("TypeError",
        std::format("{}: files must be an Object", ctx), line, col);
  }
  auto opt_str = [&](const ObjectValue& o, const char* key) -> std::string {
    if (!o.has_own(key)) return std::string();
    const Value& v = o.get(key);
    if (!v.is_stringlike()) {
      throw CulebraError("TypeError",
          std::format("{}: files {} must be a String", ctx, key), line, col);
    }
    return std::string(v.to_string_view());
  };
  auto add_part = [&](const std::string& name, const Value& v) {
    culebra::http::MultipartPart part;
    part.name = name;
    if (v.is_stringlike()) {  // bare String → plain text field.
      part.content = std::string(v.to_string_view());
      req.multipart.push_back(std::move(part));
      return;
    }
    if (v.type != Value::Object) {
      throw CulebraError("TypeError",
          std::format("{}: files['{}'] must be a String, Object, or Array",
                      ctx, name), line, col);
    }
    const ObjectValue& o = v.to_object();
    part.filename = opt_str(o, "filename");
    part.content_type = opt_str(o, "content_type");
    int sources = o.has_own("content") + o.has_own("path") + o.has_own("stream");
    if (sources != 1) {
      throw CulebraError("TypeError",
          std::format(
              "{}: files['{}'] needs exactly one of content, path, stream",
              ctx, name), line, col);
    }
    if (o.has_own("content")) {
      const Value& cv = o.get("content");
      if (!cv.is_stringlike()) {
        throw CulebraError("TypeError",
            std::format("{}: files['{}'].content must be a String", ctx, name),
            line, col);
      }
      part.content = std::string(cv.to_string_view());
    } else if (o.has_own("path")) {
      const Value& pv = o.get("path");
      if (!pv.is_stringlike()) {
        throw CulebraError("TypeError",
            std::format("{}: files['{}'].path must be a String", ctx, name),
            line, col);
      }
      std::string path(pv.to_string_view());
      part.source = culebra::http::make_file_source(path);
      if (!part.source) {
        throw CulebraError("IOError",
            std::format("{}: cannot open '{}' for reading", ctx, path), line,
            col);
      }
      if (part.filename.empty()) {
        part.filename = std::filesystem::path(path).filename().string();
      }
    } else {  // stream: producer Function
      const Value& sv = o.get("stream");
      if (sv.type != Value::Function) {
        throw CulebraError("TypeError",
            std::format("{}: files['{}'].stream must be a Function (producer)",
                        ctx, name), line, col);
      }
      if (!st.cb_interp) st.cb_interp = std::make_shared<Interpreter>();
      Value producer = sv;
      part.source = [&st, producer, env, ctx, line, col](
                        std::string& out) -> bool {
        if (st.eptr) return false;  // a previous part already failed.
        try {
          Value r = st.cb_interp->call_closure(producer, env, {});
          if (r.type == Value::Nil) return false;  // end of stream.
          if (!r.is_stringlike()) {
            throw CulebraError("TypeError",
                std::format(
                    "{}: file stream producer must return a String or nil", ctx),
                line, col);
          }
          out = std::string(r.to_string_view());
          return true;
        } catch (...) {
          st.eptr = std::current_exception();  // abort; rethrown after send.
          return false;
        }
      };
    }
    req.multipart.push_back(std::move(part));
  };
  for (const auto& [k, sym] : *filesv.to_object().properties) {
    const Value& v = sym.val;
    if (v.type == Value::Array) {
      for (const auto& el : *v.to_array().values) add_part(std::string(k), el);
    } else {
      add_part(std::string(k), v);
    }
  }
}

// Configure the request body from the `body` / `json` / `form` / `files` kwargs
// (post/put/request). `json` (non-nil) serializes the value to JSON and sends it
// as application/json. `form` sends an x-www-form-urlencoded body. `files` sends
// a multipart/form-data body (see http_setup_multipart). Otherwise `body`: a
// String is sent whole; a Function is a producer — called per chunk, returning
// the next chunk String or nil at end — streamed chunked so a big upload never
// lives in memory at once. At most one of body/json/form/files; otherwise a
// TypeError. A non-String/Function body is a TypeError.
inline void http_setup_body(const Value& bodyv, const Value& jsonv,
                            const Value& formv, const Value& filesv,
                            const Value& ct,
                            const std::shared_ptr<Environment>& env,
                            culebra::http::HttpRequest& req, HttpIntoState& st,
                            const char* ctx, int64_t line, int64_t col) {
  bool has_json = jsonv.type != Value::Nil;
  bool has_form = formv.type != Value::Nil;
  bool has_files = filesv.type != Value::Nil;
  bool has_body = bodyv.type == Value::Function ||
                  ((bodyv.type == Value::String ||
                    bodyv.type == Value::StringView) &&
                   !bodyv.to_string_view().empty());
  if (has_json + has_form + has_files + has_body > 1) {
    throw CulebraError(
        "TypeError",
        std::format("{}: pass at most one of body, json, form, files", ctx),
        line, col);
  }
  if (has_json) {
    req.body = json_stringify(jsonv, 0, false);
    req.content_type = "application/json";
    return;
  }
  if (has_form) {
    req.body = culebra::http::encode_query(
        http_parse_form(formv, ctx, line, col));
    req.content_type = "application/x-www-form-urlencoded";
    return;
  }
  if (has_files) {
    // Content-Type (multipart/form-data; boundary=...) is set by the http core.
    http_setup_multipart(filesv, env, req, st, ctx, line, col);
    return;
  }
  // nil content_type means "unset" → the default (mirrors the JIT + the other
  // optional kwargs); a present non-String still hits to_string()'s strict check.
  req.content_type = ct.type == Value::Nil ? "text/plain" : ct.to_string();
  if (bodyv.type == Value::Function) {
    if (!st.cb_interp) st.cb_interp = std::make_shared<Interpreter>();
    Value producer = bodyv;
    req.body_source = [&st, producer, env, ctx, line, col](
                          std::string& out) -> bool {
      try {
        Value r = st.cb_interp->call_closure(producer, env, {});
        if (r.type == Value::Nil) return false;  // end of stream
        if (r.type != Value::String && r.type != Value::StringView) {
          throw CulebraError(
              "TypeError",
              std::format("{}: body producer must return a String or nil", ctx),
              line, col);
        }
        out = r.to_string_view();
        return true;
      } catch (...) {
        st.eptr = std::current_exception();  // abort; rethrown after send
        return false;
      }
    };
  } else if (bodyv.type == Value::String || bodyv.type == Value::StringView) {
    req.body = bodyv.to_string_view();
  } else {
    throw CulebraError(
        "TypeError",
        std::format("{}: body must be a String or a Function (producer)", ctx),
        line, col);
  }
}

// Build an Http.client handle (id into the persistent-client registry). Methods
// mirror the free Http.* functions but take a base-url-relative `path` (no
// per-request timeout/follow_redirects — those are fixed at Http.client), reuse
// the client's one connection, and layer its default headers under each
// request. `close`/`drop` close the connection (idempotent).
inline Value make_http_client_handle(int64_t id) {
  using namespace std::literals;
  static const auto empty_str_default =
      std::make_shared<Value>(Value(std::string("")));
  static const auto text_plain_default =
      std::make_shared<Value>(Value(std::string("text/plain")));
  ObjectValue h;
  h.initialize("_id", Value(static_cast<int64_t>(id)), false);
  h.initialize("__nonsendable__", Value(true), false);

  auto path_param = FunctionValue::Parameter{"path", false, "String"sv};
  auto headers_param =
      FunctionValue::Parameter{"headers", false, ""sv, nullptr, kw_default_nil()};
  auto params_param =
      FunctionValue::Parameter{"params", false, ""sv, nullptr, kw_default_nil()};
  auto into_param =
      FunctionValue::Parameter{"into", false, ""sv, nullptr, kw_default_nil()};
  auto body_param =
      FunctionValue::Parameter{"body", false, ""sv, nullptr, empty_str_default};
  auto ct_param = FunctionValue::Parameter{"content_type", false, ""sv, nullptr,
                                           text_plain_default};
  auto json_param =
      FunctionValue::Parameter{"json", false, ""sv, nullptr, kw_default_nil()};
  auto form_param =
      FunctionValue::Parameter{"form", false, ""sv, nullptr, kw_default_nil()};
  auto files_param =
      FunctionValue::Parameter{"files", false, ""sv, nullptr, kw_default_nil()};

  auto cid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("self").to_object().get("_id").to_long();
  };

  // get / delete / head — no body.
  auto bodyless = [&](const char* method, const char* ctx) {
    return Value(FunctionValue(
        {path_param, headers_param, params_param, into_param},
        [method, ctx, cid](std::shared_ptr<Environment> env) -> Value {
          int64_t line = env->get("__LINE__").to_long();
          int64_t col = env->get("__COLUMN__").to_long();
          culebra::http::HttpRequest req;
          req.method = method;
          req.url = env->get("path").to_string();
          req.headers = http_parse_headers(env->get("headers"), ctx, line, col);
          req.params = http_parse_params(env->get("params"), ctx, line, col);
          HttpIntoState st;
          http_setup_into(env->get("into"), env, req, st, ctx, line, col);
          return http_run_client_into(cid(env), req, st, ctx, line, col);
        },
        "Object"sv));
  };
  h.initialize("get", bodyless("GET", "client.get"), false);
  h.initialize("delete", bodyless("DELETE", "client.delete"), false);
  h.initialize("head", bodyless("HEAD", "client.head"), false);

  // post / put — body + content_type.
  auto withbody = [&](const char* method, const char* ctx) {
    return Value(FunctionValue(
        {path_param, body_param, ct_param, headers_param, params_param,
         into_param, json_param, form_param, files_param},
        [method, ctx, cid](std::shared_ptr<Environment> env) -> Value {
          int64_t line = env->get("__LINE__").to_long();
          int64_t col = env->get("__COLUMN__").to_long();
          culebra::http::HttpRequest req;
          req.method = method;
          req.url = env->get("path").to_string();
          req.headers = http_parse_headers(env->get("headers"), ctx, line, col);
          req.params = http_parse_params(env->get("params"), ctx, line, col);
          HttpIntoState st;
          http_setup_body(env->get("body"), env->get("json"), env->get("form"),
                          env->get("files"), env->get("content_type"), env, req,
                          st, ctx, line, col);
          http_setup_into(env->get("into"), env, req, st, ctx, line, col);
          return http_run_client_into(cid(env), req, st, ctx, line, col);
        },
        "Object"sv));
  };
  h.initialize("post", withbody("POST", "client.post"), false);
  h.initialize("put", withbody("PUT", "client.put"), false);

  // request(method, path, ...) — generic escape hatch (PATCH, OPTIONS, …).
  h.initialize(
      "request",
      Value(FunctionValue(
          {{"method", false, "String"sv}, path_param, body_param, ct_param,
           headers_param, params_param, into_param, json_param, form_param,
           files_param},
          [cid](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            culebra::http::HttpRequest req;
            req.method = env->get("method").to_string();
            req.url = env->get("path").to_string();
            req.headers =
                http_parse_headers(env->get("headers"), "client.request", line, col);
            req.params =
                http_parse_params(env->get("params"), "client.request", line, col);
            HttpIntoState st;
            http_setup_body(env->get("body"), env->get("json"), env->get("form"),
                            env->get("files"), env->get("content_type"), env, req,
                            st, "client.request", line, col);
            http_setup_into(env->get("into"), env, req, st, "client.request",
                            line, col);
            return http_run_client_into(cid(env), req, st, "client.request",
                                        line, col);
          },
          "Object"sv)),
      false);

  // close — release the connection. drop — GC backstop for the same.
  h.initialize(
      "close",
      Value(FunctionValue({}, [cid](std::shared_ptr<Environment> env) {
        culebra::http::http_client_close(cid(env));
        return Value();
      })),
      false);
  h.initialize(
      "drop",
      Value(FunctionValue({}, [cid](std::shared_ptr<Environment> env) {
        culebra::http::http_client_close(cid(env));
        return Value();
      })),
      false);

  return Value(std::move(h));
}

// Build the `req` Object handed to a server handler from an httplib request:
// method/path/body as Strings, and headers/query/params as Objects of String.
// `query` is the parsed query string; `params` the matched route path
// parameters (e.g. ":id" → req.params["id"]). Express-style naming.
inline Value http_request_to_object(const httplib::Request& q) {
  ObjectValue req;
  req.initialize("method", Value(std::string(q.method)), false);
  req.initialize("path", Value(std::string(q.path)), false);
  req.initialize("body", Value(std::string(q.body)), false);
  ObjectValue headers;
  for (const auto& [k, v] : q.headers)
    headers.initialize(k, Value(std::string(v)), false);
  req.initialize("headers", Value(std::move(headers)), false);
  ObjectValue query;
  for (const auto& [k, v] : q.params)
    query.initialize(k, Value(std::string(v)), false);
  req.initialize("query", Value(std::move(query)), false);
  ObjectValue params;
  for (const auto& [k, v] : q.path_params)
    params.initialize(k, Value(std::string(v)), false);
  req.initialize("params", Value(std::move(params)), false);
  return Value(std::move(req));
}

// Apply a response Object's status/content_type/headers to `res` and return the
// resolved content_type. Shared by the buffered path (http_apply_response) and
// the streaming path (http_try_stream_response); the body/stream differs, this
// metadata does not. status/content_type read via to_long/to_string_view, which
// reject a present, non-nil field of the wrong type with a TypeError.
inline std::string http_apply_response_meta(const ObjectValue& obj,
                                            httplib::Response& res) {
  int64_t status = 200;
  if (obj.has_own("status") && obj.get("status").type != Value::Nil) {
    status = obj.get("status").to_long();
  }
  res.status = static_cast<int>(status);
  std::string content_type = "text/plain";
  if (obj.has_own("content_type") &&
      obj.get("content_type").type != Value::Nil) {
    content_type = std::string(obj.get("content_type").to_string_view());
  }
  if (obj.has_own("headers") && obj.get("headers").type == Value::Object) {
    for (const auto& [k, sym] : *obj.get("headers").to_object().properties) {
      res.set_header(std::string(k), std::string(sym.val.to_string_view()));
    }
  }
  return content_type;
}

// Apply a handler's return value to the httplib response:
//   String   → 200, text/plain, body = string
//   Object   → {status?, body?, headers?, content_type?}; absent → defaults
//              (200 / "" / text/plain). headers is an Object of String.
//   nil      → 200, empty body
// Anything else is a TypeError (turned into a 500 by the trampoline's catch).
inline void http_apply_response(const Value& ret, httplib::Response& res) {
  if (ret.type == Value::Nil) {
    res.status = 200;
    return;
  }
  if (ret.type == Value::String || ret.type == Value::StringView) {
    res.status = 200;
    res.set_content(std::string(ret.to_string_view()), "text/plain");
    return;
  }
  if (ret.type == Value::Object) {
    const auto& obj = ret.to_object();
    std::string content_type = http_apply_response_meta(obj, res);
    std::string body;
    if (obj.has_own("body") && obj.get("body").type != Value::Nil) {
      body = std::string(obj.get("body").to_string_view());
    }
    res.set_content(body, content_type);
    return;
  }
  throw CulebraError(
      "TypeError",
      "Http.server: handler must return a String, an Object, or nil", 0, 0);
}

// The `sink` handle handed to a streaming handler's closure: sink.write(chunk)
// pushes one chunk to the client and returns false if the client has gone away
// (so the handler can stop early). Backed by an id (slot + generation) into the
// value-neutral sink registry; valid only during the stream callback — a
// captured/escaped sink resolves stale and write() returns false.
inline Value make_http_sink_handle(int64_t sink_id) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("_sink", Value(static_cast<int64_t>(sink_id)), false);
  h.initialize("__nonsendable__", Value(true), false);
  h.initialize(
      "write",
      Value(FunctionValue(
          {{"data", false, "String"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t sid = env->get("self").to_object().get("_sink").to_long();
            auto sv = env->get("data").to_string_view();
            return Value(http::http_sink_write(sid, sv.data(), sv.size()));
          },
          "Bool"sv)),
      false);
  return Value(std::move(h));
}

// If `ret` is an Object carrying a `stream` Function, wire a chunked response
// (status/content_type/headers from the same Object apply, mirroring
// http_apply_response) and return true; the caller then skips
// http_apply_response. The provider invokes `stream` with a sink handle on this
// worker thread. body + stream together is a TypeError. Returns false for any
// non-streaming response. A mid-stream exception aborts the connection (the
// status line is already sent, so it cannot become a 500).
inline bool http_try_stream_response(const Value& ret, httplib::Response& res,
                                     std::shared_ptr<Interpreter> interp,
                                     std::shared_ptr<Environment> env) {
  if (ret.type != Value::Object) return false;
  const auto& obj = ret.to_object();
  if (!(obj.has_own("stream") && obj.get("stream").type != Value::Nil))
    return false;
  Value stream_fn = obj.get("stream");
  if (stream_fn.type != Value::Function)
    throw CulebraError("TypeError",
                       "Http.server: response stream must be a Function", 0, 0);
  if (obj.has_own("body") && obj.get("body").type != Value::Nil)
    throw CulebraError("TypeError",
                       "Http.server: response cannot set both body and stream",
                       0, 0);
  std::string content_type = http_apply_response_meta(obj, res);
  // Capturing stream_fn (a Value copy = +1) keeps the closure alive until the
  // provider runs after this returns (same worker thread, same response write).
  http::http_server_set_stream(
      res, content_type, [interp, env, stream_fn](int64_t sink_id) -> bool {
        try {
          interp->call_closure(stream_fn, env, {make_http_sink_handle(sink_id)});
          return true;
        } catch (...) {
          return false;
        }
      });
  return true;
}

// The `ws` handle, for both a server handler's connection and an Http.ws client.
// send(msg)->Bool, receive()->String|nil (nil when the peer closes), close(),
// is_open()->Bool, and `iter` so `for msg in ws { ... }` drains inbound messages
// until close. Backed by a WsConn id (slot+generation); a server ws is valid
// only during its handler. A client adds `drop` to close + free the owned
// connection (a server ws is borrowed — the trampoline frees it).
inline Value make_http_ws_handle(int64_t ws_id, bool is_client) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("_ws", Value(static_cast<int64_t>(ws_id)), false);
  h.initialize("__nonsendable__", Value(true), false);
  auto wid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("self").to_object().get("_ws").to_long();
  };
  h.initialize(
      "send",
      Value(FunctionValue(
          {{"data", false, "String"sv}},
          [wid](std::shared_ptr<Environment> env) -> Value {
            auto sv = env->get("data").to_string_view();
            return Value(http::ws_send(wid(env), sv.data(), sv.size()));
          },
          "Bool"sv)),
      false);
  h.initialize(
      "receive",
      Value(FunctionValue({}, [wid](std::shared_ptr<Environment> env) -> Value {
        std::string out;
        if (http::ws_receive(wid(env), out) == 0) return Value();  // nil = closed
        return Value(std::move(out));
      })),
      false);
  h.initialize(
      "close",
      Value(FunctionValue({}, [wid](std::shared_ptr<Environment> env) -> Value {
        http::ws_close(wid(env));
        return Value();
      })),
      false);
  h.initialize(
      "is_open",
      Value(FunctionValue(
          {},
          [wid](std::shared_ptr<Environment> env) -> Value {
            return Value(http::ws_is_open(wid(env)));
          },
          "Bool"sv)),
      false);
  h.initialize(
      "iter",
      Value(FunctionValue({}, [wid](std::shared_ptr<Environment> env) -> Value {
        int64_t id = wid(env);
        return _make_iterator(
            [id](std::shared_ptr<Environment>) -> std::optional<Value> {
              std::string out;
              if (http::ws_receive(id, out) == 0) return _iter_step_done();
              return _iter_step_value(Value(std::move(out)));
            });
      })),
      false);
  if (is_client) {
    h.initialize(
        "drop",
        Value(FunctionValue({}, [wid](std::shared_ptr<Environment> env) -> Value {
          http::ws_unregister(wid(env));
          return Value();
        })),
        false);
  }
  return Value(std::move(h));
}

// A route recorded by get/post/… and registered at listen() time — deferred so
// the worker count (and thus whether handlers must be Sendable) is known before
// we choose the inline vs. worker-pool path. Keyed by the handle's id;
// thread_local because the handle is non-sendable.
struct InterpRouteRecord {
  std::string method;
  std::string pattern;
  Value handler;
};
inline thread_local std::unordered_map<int64_t, std::vector<InterpRouteRecord>>
    g_srv_routes;

// Defined after isolate.h is included (it needs sendable::serialize, since
// handlers always run on a worker pool); forward-declared here so the serve
// handle methods can call it. Registers the routes and serves; returns nil, or
// throws on a Sendable / serve error. `ctx` names the method the user called
// ("server.serve" / "server.listen") so the error says where it came from.
inline Value _http_server_do_serve(int64_t id, int64_t workers, bool async,
                                   const char* ctx, int64_t line, int64_t col);

// `nil` means "unset" for the optional listen/serve params, as elsewhere in the
// Http module — the declared defaults cover an omitted argument, these cover an
// explicit nil.
inline std::string _http_server_host_arg(const Value& v) {
  return v.type == Value::Nil ? std::string("0.0.0.0")
                              : std::string(v.to_string_view());
}
inline int64_t _http_server_workers_arg(const Value& v) {
  return v.type == Value::Nil ? 0 : v.to_long();  // 0 = CPU-scaled
}
inline int64_t _http_server_bind_or_throw(int64_t id, const std::string& host,
                                          int port, const char* ctx,
                                          int64_t line, int64_t col) {
  std::string err;
  int bound = culebra::http::http_server_bind(id, host, port, err);
  if (bound < 0)
    throw CulebraError("HttpError", std::format("{}: {}", ctx, err), line, col);
  return bound;
}

// The Http.server handle: record routes (get/post/…) and serve files (static),
// then bind() to open the socket and serve() to accept on it — or listen(),
// which does both. Handlers always run on a worker pool (each worker thread gets
// its own runtime with the handlers rebuilt onto it), so they must be Sendable;
// the accept loop never runs a handler. `workers` defaults to 0 = a CPU-scaled
// pool size (see default_http_workers); pass a positive count to fix it.
inline Value make_http_server_handle(int64_t id) {
  using namespace std::literals;
  static const auto host_default =
      std::make_shared<Value>(Value(std::string("0.0.0.0")));
  const auto& workers_default = kw_default_zero();  // 0 = CPU-scaled
  ObjectValue h;
  h.initialize("_id", Value(static_cast<int64_t>(id)), false);
  h.initialize("__nonsendable__", Value(true), false);

  auto sid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("self").to_object().get("_id").to_long();
  };

  // get/post/put/delete/patch/options(pattern, handler) — record a route (the
  // handler is `fn(req) -> response`); registered at listen(). Chainable.
  auto route = [&](const char* method) {
    return Value(FunctionValue(
        {{"pattern", false, "String"sv}, {"handler", false, "Function"sv}},
        [method, sid](std::shared_ptr<Environment> env) -> Value {
          g_srv_routes[sid(env)].push_back(
              {method, env->get("pattern").to_string(), env->get("handler")});
          return env->get("self");
        },
        "Object"sv));
  };
  h.initialize("get", route("GET"), false);
  h.initialize("post", route("POST"), false);
  h.initialize("put", route("PUT"), false);
  h.initialize("delete", route("DELETE"), false);
  h.initialize("patch", route("PATCH"), false);
  h.initialize("options", route("OPTIONS"), false);
  // ws(pattern, handler) — register a WebSocket route; the handler is
  // `fn(req, ws)` and runs a long-lived loop for the connection. Same recording
  // path as the HTTP routes (method "WS"); dispatched specially at listen().
  h.initialize("ws", route("WS"), false);

  // static(mount, dir) — serve static files at the URL prefix `mount`. `dir` is
  // either a String path (a live on-disk directory, via httplib's mount point)
  // or an `Embed.dir(...)` handle (baked into the binary under AOT, live
  // disk otherwise). Static files are tried before routes and only when present,
  // so API routes still win. Chainable.
  h.initialize(
      "static",
      Value(FunctionValue(
          {{"mount", false, "String"sv}, {"dir", false, ""sv}},
          [sid](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            std::string err;
            std::string mount = std::string(env->get("mount").to_string_view());
            const Value& d = env->get("dir");
            // A real Embed.dir handle has both __embed_dir__ and name; a
            // forged lookalike missing either falls through to the TypeError
            // below (not an uncaught out_of_range from get("name")).
            if (d.type == Value::Object &&
                d.to_object().has("__embed_dir__") &&
                d.to_object().has("name")) {
              const auto& o = d.to_object();
              culebra::http::http_server_serve_embed(
                  sid(env), mount,
                  std::string(o.get("name").to_string_view()), err);
            } else if (d.type == Value::String ||
                       d.type == Value::StringView) {
              culebra::http::http_server_static(
                  sid(env), mount, std::string(d.to_string_view()), err);
            } else {
              throw CulebraError(
                  "TypeError",
                  "server.static: dir must be a String path or Embed.dir(...)",
                  line, col);
            }
            if (!err.empty()) {
              throw CulebraError("HttpError",
                                 std::format("server.static: {}", err), line,
                                 col);
            }
            return env->get("self");
          },
          "Object"sv)),
      false);

  // bind(port, host="0.0.0.0") — open the listening socket and return the port
  // it got (port 0 = an OS-chosen one). Routes may be registered either side of
  // it; only serve() needs them in place.
  h.initialize(
      "bind",
      Value(FunctionValue(
          {{"port", false, ""sv}, {"host", false, ""sv, nullptr, host_default}},
          [sid](std::shared_ptr<Environment> env) -> Value {
            return Value(_http_server_bind_or_throw(
                sid(env), _http_server_host_arg(env->get("host")),
                static_cast<int>(env->get("port").to_long()), "server.bind",
                env->get("__LINE__").to_long(),
                env->get("__COLUMN__").to_long()));
          },
          "Long"sv)),
      false);

  // serve(workers=0) / serve_async(workers=0) — register the routes and run the
  // accept loop on a bound socket. `async` false blocks until Ctrl+C; true
  // returns immediately and serves on a background thread (stop with stop()).
  std::vector<FunctionValue::Parameter> serve_params = {
      {"workers", false, ""sv, nullptr, workers_default}};
  auto do_serve = [sid](bool async) {
    return [sid, async](std::shared_ptr<Environment> env) -> Value {
      return _http_server_do_serve(
          sid(env), _http_server_workers_arg(env->get("workers")), async,
          async ? "server.serve_async" : "server.serve",
          env->get("__LINE__").to_long(), env->get("__COLUMN__").to_long());
    };
  };
  h.initialize("serve",
               Value(FunctionValue(serve_params, do_serve(false), "Nil"sv)),
               false);
  h.initialize("serve_async",
               Value(FunctionValue(serve_params, do_serve(true), "Nil"sv)),
               false);

  // listen(port, host, workers) — bind + serve in one call. The blocking form
  // returns nil (it only returns once stopped); the async form returns the bound
  // port, which is the only way to learn it when port 0 was asked for.
  auto do_listen = [sid](bool async) {
    return [sid, async](std::shared_ptr<Environment> env) -> Value {
      int64_t line = env->get("__LINE__").to_long();
      int64_t col = env->get("__COLUMN__").to_long();
      const char* ctx = async ? "server.listen_async" : "server.listen";
      int64_t self_id = sid(env);
      int64_t bound = _http_server_bind_or_throw(
          self_id, _http_server_host_arg(env->get("host")),
          static_cast<int>(env->get("port").to_long()), ctx, line, col);
      Value r = _http_server_do_serve(
          self_id, _http_server_workers_arg(env->get("workers")), async, ctx,
          line, col);
      return async ? Value(bound) : r;
    };
  };
  std::vector<FunctionValue::Parameter> listen_params = {
      {"port", false, ""sv},
      {"host", false, ""sv, nullptr, host_default},
      {"workers", false, ""sv, nullptr, workers_default}};
  // listen(port, host="0.0.0.0", workers=0) — blocks until Ctrl+C. workers=0
  // picks a CPU-scaled pool; handlers run on the pool (must be Sendable).
  h.initialize("listen", Value(FunctionValue(listen_params, do_listen(false),
                                             "Nil"sv)),
               false);
  // listen_async(port, host="0.0.0.0", workers=0) -> Long — returns the bound
  // port at once; handlers must be Sendable (they run off this thread).
  h.initialize("listen_async",
               Value(FunctionValue(listen_params, do_listen(true), "Long"sv)),
               false);
  // stop() — stop a background (listen_async) server and join its thread.
  h.initialize(
      "stop",
      Value(FunctionValue({}, [](std::shared_ptr<Environment> env) -> Value {
        culebra::http::http_server_stop(
            env->get("self").to_object().get("_id").to_long());
        return Value();
      })),
      false);

  // close — stop, drop recorded routes, free. drop — GC backstop for the same.
  auto shutdown = [](std::shared_ptr<Environment> env) {
    int64_t id = env->get("self").to_object().get("_id").to_long();
    g_srv_routes.erase(id);
    culebra::http::http_server_close(id);
    return Value();
  };
  h.initialize("close", Value(FunctionValue({}, shutdown)), false);
  h.initialize("drop", Value(FunctionValue({}, shutdown)), false);

  return Value(std::move(h));
}

// `Http` — synchronous HTTP/HTTPS client (cpp-httplib + OpenSSL). Each call
// blocks until the response arrives. A 4xx/5xx is a normal result (`ok:false`);
// a transport failure (DNS/connect/TLS/timeout) throws HttpError.
inline Value make_http_namespace() {
  using namespace std::literals;
  static const auto empty_str_default =
      std::make_shared<Value>(Value(std::string("")));
  static const auto text_plain_default =
      std::make_shared<Value>(Value(std::string("text/plain")));
  ObjectValue ns;

  auto url_param = FunctionValue::Parameter{"url", false, "String"sv};
  auto headers_param =
      FunctionValue::Parameter{"headers", false, ""sv, nullptr, kw_default_nil()};
  auto timeout_param =
      FunctionValue::Parameter{"timeout", false, ""sv, nullptr, kw_default_zero()};
  auto follow_param = FunctionValue::Parameter{"follow_redirects", false, ""sv,
                                           nullptr, kw_default_true()};
  auto body_param =
      FunctionValue::Parameter{"body", false, ""sv, nullptr, empty_str_default};
  auto ct_param = FunctionValue::Parameter{"content_type", false, ""sv, nullptr,
                                       text_plain_default};
  // `into` (response sink): nil → buffer body; String → file path; Function →
  // per-chunk callback. Streams the response, leaving the result's body empty.
  auto into_param =
      FunctionValue::Parameter{"into", false, ""sv, nullptr, kw_default_nil()};
  // `params` (query string): an Object of String values, appended to the URL
  // percent-encoded.
  auto params_param =
      FunctionValue::Parameter{"params", false, ""sv, nullptr, kw_default_nil()};
  // `json` (post/put/request): serialize the value to JSON and send it as
  // application/json. Mutually exclusive with `body`.
  auto json_param =
      FunctionValue::Parameter{"json", false, ""sv, nullptr, kw_default_nil()};
  // `form` (post/put/request): an Object of String values sent as an
  // application/x-www-form-urlencoded body. Mutually exclusive with body/json.
  auto form_param =
      FunctionValue::Parameter{"form", false, ""sv, nullptr, kw_default_nil()};
  // `files` (post/put/request): an Object of parts sent as multipart/form-data
  // (text fields, in-memory/disk/producer file parts). Mutually exclusive with
  // body/json/form.
  auto files_param =
      FunctionValue::Parameter{"files", false, ""sv, nullptr, kw_default_nil()};

  // GET / DELETE / HEAD — no body.
  auto bodyless = [&](const char* method, const char* ctx) {
    return Value(FunctionValue(
        {url_param, headers_param, timeout_param, follow_param, into_param,
         params_param},
        [method, ctx](std::shared_ptr<Environment> env) -> Value {
          int64_t line = env->get("__LINE__").to_long();
          int64_t col = env->get("__COLUMN__").to_long();
          culebra::http::HttpRequest req;
          req.method = method;
          req.url = env->get("url").to_string();
          http_fill_common(env, req, ctx, line, col);
          HttpIntoState st;
          http_setup_into(env->get("into"), env, req, st, ctx, line, col);
          return http_run_into(req, st, ctx, line, col);
        },
        "Object"sv));
  };
  ns.initialize("get", bodyless("GET", "Http.get"), false);
  ns.initialize("delete", bodyless("DELETE", "Http.delete"), false);
  ns.initialize("head", bodyless("HEAD", "Http.head"), false);

  // POST / PUT — body + content_type.
  auto withbody = [&](const char* method, const char* ctx) {
    return Value(FunctionValue(
        {url_param, body_param, ct_param, headers_param, timeout_param,
         follow_param, into_param, params_param, json_param, form_param,
         files_param},
        [method, ctx](std::shared_ptr<Environment> env) -> Value {
          int64_t line = env->get("__LINE__").to_long();
          int64_t col = env->get("__COLUMN__").to_long();
          culebra::http::HttpRequest req;
          req.method = method;
          req.url = env->get("url").to_string();
          http_fill_common(env, req, ctx, line, col);
          HttpIntoState st;
          http_setup_body(env->get("body"), env->get("json"),
                          env->get("form"), env->get("files"),
                          env->get("content_type"), env, req, st, ctx, line,
                          col);
          http_setup_into(env->get("into"), env, req, st, ctx, line, col);
          return http_run_into(req, st, ctx, line, col);
        },
        "Object"sv));
  };
  ns.initialize("post", withbody("POST", "Http.post"), false);
  ns.initialize("put", withbody("PUT", "Http.put"), false);

  // `Http.request(method, url, body="", content_type="text/plain",
  //               headers=nil, timeout=0, follow_redirects=true, into=nil)` —
  // generic escape hatch for any method (PATCH, OPTIONS, ...).
  ns.initialize(
      "request",
      Value(FunctionValue(
          {{"method", false, "String"sv}, url_param, body_param, ct_param,
           headers_param, timeout_param, follow_param, into_param,
           params_param, json_param, form_param, files_param},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            culebra::http::HttpRequest req;
            req.method = env->get("method").to_string();
            req.url = env->get("url").to_string();
            http_fill_common(env, req, "Http.request", line, col);
            HttpIntoState st;
            http_setup_body(env->get("body"), env->get("json"),
                            env->get("form"), env->get("files"),
                            env->get("content_type"), env, req, st,
                            "Http.request", line, col);
            http_setup_into(env->get("into"), env, req, st, "Http.request",
                            line, col);
            return http_run_into(req, st, "Http.request", line, col);
          },
          "Object"sv)),
      false);

  // `Http.sse(url, on_event, headers=nil, timeout=0, follow_redirects=true)` —
  // open a Server-Sent Events (text/event-stream) GET and call `on_event` with
  // each event Object `{event, data, id}` as it arrives. Blocks for the life of
  // the stream. The callback runs on the calling thread (may mutate captured
  // state); returning/throwing from it ends the stream (a throw propagates).
  ns.initialize(
      "sse",
      Value(FunctionValue(
          {url_param,
           FunctionValue::Parameter{"on_event", false, "Function"sv},
           headers_param, timeout_param, follow_param},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            culebra::http::HttpRequest req;
            req.method = "GET";
            req.url = env->get("url").to_string();
            const Value cb = env->get("on_event");
            req.headers =
                http_parse_headers(env->get("headers"), "Http.sse", line, col);
            const auto& tv = env->get("timeout");
            int64_t timeout = tv.type == Value::Nil ? 0 : tv.to_long();
            req.timeout_sec = timeout > 0 ? timeout : 0;
            const auto& fv = env->get("follow_redirects");
            req.follow_redirects = fv.type == Value::Nil ? true : fv.to_bool();
            bool has_accept = false;
            for (auto& [k, v] : req.headers) {
              if (culebra::http::_iequals(k, "Accept")) has_accept = true;
            }
            if (!has_accept) {
              req.headers.emplace_back("Accept", "text/event-stream");
            }
            auto cb_interp = std::make_shared<Interpreter>();
            std::exception_ptr eptr;
            culebra::http::SseDecoder dec;
            dec.handler = [&](const culebra::http::SseEvent& e) -> bool {
              try {
                ObjectValue ev;
                ev.initialize("event", Value(std::string(e.type)), false);
                ev.initialize("data", Value(std::string(e.data)), false);
                ev.initialize("id", Value(std::string(e.id)), false);
                cb_interp->call_closure(cb, env, {Value(std::move(ev))});
                return true;
              } catch (...) {
                eptr = std::current_exception();
                return false;
              }
            };
            req.body_sink = [&dec](const char* d, size_t n) {
              return dec.feed(d, n);
            };
            auto r = culebra::http::http_request(req);
            if (eptr) std::rethrow_exception(eptr);
            if (!r.ok) {
              throw CulebraError(
                  "HttpError", std::format("Http.sse: {}", r.error), line, col);
            }
            return http_result_to_value(std::move(r));
          },
          "Object"sv)),
      false);

  // `Http.client(base_url, headers=nil, timeout=0, follow_redirects=true)` — a
  // persistent client reusing one keep-alive connection, with a base URL and
  // default headers. Returns a handle whose get/post/put/delete/head/request
  // take a base-url-relative path and reuse the connection; close() releases it.
  ns.initialize(
      "client",
      Value(FunctionValue(
          {{"base_url", false, "String"sv}, headers_param, timeout_param,
           follow_param},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            auto headers =
                http_parse_headers(env->get("headers"), "Http.client", line, col);
            const auto& tv = env->get("timeout");
            int64_t timeout = tv.type == Value::Nil ? 0 : tv.to_long();
            const auto& fv = env->get("follow_redirects");
            bool follow = fv.type == Value::Nil ? true : fv.to_bool();
            std::string err;
            int64_t id = culebra::http::http_client_open(
                env->get("base_url").to_string(), std::move(headers),
                timeout > 0 ? timeout : 0, follow, err);
            if (id < 0) {
              throw CulebraError("HttpError",
                  std::format("Http.client: {}", err), line, col);
            }
            return make_http_client_handle(id);
          },
          "Object"sv)),
      false);

  // `Http.server()` — an HTTP server. Register routes with get/post/put/delete/
  // patch/options(pattern, fn(req)->response) and serve files with
  // static(mount, dir), then listen(port, host="0.0.0.0") to block and accept
  // connections (Ctrl+C to stop). v0 is single threaded.
  ns.initialize(
      "server",
      Value(FunctionValue(
          {},
          [](std::shared_ptr<Environment>) -> Value {
            return make_http_server_handle(culebra::http::http_server_open());
          },
          "Object"sv)),
      false);

  // `Http.ws(url)` — connect a WebSocket client to `url` (ws://host:port/path)
  // and return a handle (send/receive/close/is_open, and `for msg in ws`). A
  // bad URL or failed connect is an HttpError.
  ns.initialize(
      "ws",
      Value(FunctionValue(
          {{"url", false, "String"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            std::string err;
            int64_t id = culebra::http::ws_client_open(
                env->get("url").to_string(), err);
            if (id < 0) throw CulebraError("HttpError", err, 0, 0);
            return make_http_ws_handle(id, /*is_client=*/true);
          },
          "Object"sv)),
      false);

  return Value(std::move(ns));
}
#endif  // CULEBRA_HTTP_ENABLED

// `Embed` — bake a directory of assets into the program. `Embed.dir(name)`
// returns a handle over that directory: under an AOT build the named directory
// is walked at build time and its bytes are linked into the binary; running
// from source it reads the live on-disk directory (relative to the entry
// script) so an edit shows up on the next read. `read`/`exists` take a file out
// of it directly; `srv.static(mount, ...)` serves the whole thing. The handle
// carries its `name` as data and stays Sendable (sendable.h rebuilds it from
// that name, as it does for the other native handles it ships by reference).
inline Value make_embed_dir_handle(std::string name) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("__embed_dir__", Value(true), false);
  h.initialize("name", Value(std::move(name)), false);

  // The directory this handle names, for an error message.
  auto dir_name = [](std::shared_ptr<Environment>& env) {
    return std::string(env->get("self").to_object().get("name").to_string_view());
  };
  h.initialize(
      "read",
      Value(FunctionValue(
          {{"path", false, "String"sv}},
          [dir_name](std::shared_ptr<Environment> env) -> Value {
            std::string name = dir_name(env);
            auto path = env->get("path").to_string_view();
            std::string bytes;
            if (!culebra::embed_dir_read(name, path, bytes)) {
              _io_throw(std::format("Embed.dir.read: '{}' has no '{}'", name,
                                    path),
                        env->get("__LINE__").to_long(),
                        env->get("__COLUMN__").to_long());
            }
            return Value(std::move(bytes));
          },
          "String"sv)),
      false);
  h.initialize(
      "exists",
      Value(FunctionValue(
          {{"path", false, "String"sv}},
          [dir_name](std::shared_ptr<Environment> env) -> Value {
            return Value(culebra::embed_dir_exists(
                dir_name(env), env->get("path").to_string_view()));
          },
          "Bool"sv)),
      false);
  return Value(std::move(h));
}

inline Value make_embed_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize(
      "dir",
      Value(FunctionValue(
          {{"name", false, "String"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            return make_embed_dir_handle(
                std::string(env->get("name").to_string_view()));
          },
          "Object"sv)),
      false);
  return Value(std::move(ns));
}

// `Proc.run(cmd, cwd=nil, env=nil, stdin="", check=false)` — run an external
// command synchronously. `cmd` is a non-empty Array<String> (no shell). A
// non-zero exit or signal death is a normal result (`ok:false`); a spawn
// failure (or `check:true` on failure) throws ProcessError.
inline Value make_proc_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  static const auto proc_stdin_default =
      std::make_shared<Value>(Value(std::string("")));
  ns.initialize(
      "run",
      Value(FunctionValue(
          {
              {"cmd", false, "Array"sv},
              {"cwd", false, "String?"sv, nullptr, kw_default_nil()},
              {"env", false, "Object?"sv, nullptr, kw_default_nil()},
              {"stdin", false, "String"sv, nullptr, proc_stdin_default},
              {"check", false, "Bool"sv, nullptr, kw_default_false()},
              {"timeout", false, "Long"sv, nullptr, kw_default_zero()},
              {"share", false, ""sv, nullptr, kw_default_nil()},
          },
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();

            auto la = proc_parse_launch(env, "Proc.run", line, col);
            bool check = env->get("check").to_bool();
            int64_t timeout = env->get("timeout").to_long();
            if (timeout < 0) timeout = 0;

            auto oc = culebra::proc::run_command(la.argv, la.cwd_ptr(),
                                                 la.env_ptr(), la.stdin_data,
                                                 timeout, la.share_ptr());
            if (!oc.spawned) {
              throw CulebraError("ProcessError",
                  std::format("Proc.run: {} failed: {}.", oc.err_what,
                              std::system_category().message(oc.err_no)),
                  line, col);
            }
            if (check && !oc.result.ok) {
              throw CulebraError("ProcessError",
                  std::format("Proc.run: command {}",
                              culebra::proc::failure_detail(oc.result)),
                  line, col);
            }
            return proc_outcome_to_value(std::move(oc));
          },
          "Object"sv)),
      false);

  // `Proc.all(commands, limit=0, timeout=0, fail_fast=false)` — run each command
  // (an Array<String>) with at most `limit` concurrent children (0 => CPU
  // count). Default is allSettled: returns an Array of result Objects in input
  // order; a spawn failure is a result with `ok:false`/`error`, never a throw.
  // `fail_fast: true` instead kills the rest and throws ProcessError on the
  // first failure (Promise.all semantics).
  ns.initialize(
      "all",
      Value(FunctionValue(
          {
              {"commands", false, "Array"sv},
              {"limit", false, "Long"sv, nullptr, kw_default_zero()},
              {"timeout", false, "Long"sv, nullptr, kw_default_zero()},
              {"fail_fast", false, "Bool"sv, nullptr, kw_default_false()},
              {"retries", false, "Long"sv, nullptr, kw_default_zero()},
              {"share", false, ""sv, nullptr, kw_default_nil()},
          },
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            auto commands = proc_parse_command_list(
                *env->get("commands").to_array().values, "Proc.all", line, col);
            int64_t lim = env->get("limit").to_long();
            if (lim < 0) lim = 0;
            int64_t timeout = env->get("timeout").to_long();
            if (timeout < 0) timeout = 0;
            bool fail_fast = env->get("fail_fast").to_bool();
            int64_t retries = env->get("retries").to_long();
            if (retries < 0) retries = 0;
            std::vector<std::pair<std::string, std::string>> share_env;
            std::vector<int> share_fds;
            proc_parse_share(env->get("share"), "Proc.all", line, col,
                             share_env, share_fds);
            size_t failed = SIZE_MAX;
            auto outcomes = culebra::proc::run_all(
                commands, static_cast<size_t>(lim), nullptr,
                share_env.empty() ? nullptr : &share_env, nullptr,
                timeout, fail_fast, &failed, retries,
                share_fds.empty() ? nullptr : &share_fds);
            if (fail_fast && failed != SIZE_MAX) {
              throw CulebraError("ProcessError",
                  std::format("Proc.all: command {} {}", failed,
                              culebra::proc::outcome_detail(outcomes[failed])),
                  line, col);
            }
            ArrayValue av;
            av.values->reserve(outcomes.size());
            for (auto& oc : outcomes) {
              av.values->emplace_back(proc_outcome_to_value(std::move(oc)));
            }
            return Value(std::move(av));
          },
          "Array"sv)),
      false);

  // `Proc.race(commands)` — start every command, return the first to finish
  // and SIGKILL the rest. Returns a single result Object (the winner's; if it
  // failed to spawn, `ok:false` + `error`). Empty list throws ValueError.
  ns.initialize(
      "race",
      Value(FunctionValue(
          {
              {"commands", false, "Array"sv},
              {"share", false, ""sv, nullptr, kw_default_nil()},
          },
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            auto commands = proc_parse_command_list(
                *env->get("commands").to_array().values, "Proc.race", line,
                col);
            if (commands.empty()) {
              throw CulebraError("ValueError",
                  "Proc.race: empty command list", line, col);
            }
            std::vector<std::pair<std::string, std::string>> share_env;
            std::vector<int> share_fds;
            proc_parse_share(env->get("share"), "Proc.race", line, col,
                             share_env, share_fds);
            auto [winner, oc] = culebra::proc::run_race(
                commands, 0, nullptr,
                share_env.empty() ? nullptr : &share_env, nullptr,
                share_fds.empty() ? nullptr : &share_fds);
            (void)winner;
            return proc_outcome_to_value(std::move(oc));
          },
          "Object"sv)),
      false);

  // `Proc.spawn(cmd, cwd=nil, env=nil, stdin="")` — start a command and return
  // a live handle (`wait`/`poll`/`kill`) without waiting for it. A spawn
  // failure throws ProcessError.
  ns.initialize(
      "spawn",
      Value(FunctionValue(
          {
              {"cmd", false, "Array"sv},
              {"cwd", false, "String?"sv, nullptr, kw_default_nil()},
              {"env", false, "Object?"sv, nullptr, kw_default_nil()},
              {"stdin", false, "String"sv, nullptr, proc_stdin_default},
              {"share", false, ""sv, nullptr, kw_default_nil()},
          },
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            auto la = proc_parse_launch(env, "Proc.spawn", line, col);
            auto sr = culebra::proc::spawn_detached(la.argv, la.cwd_ptr(),
                                                    la.env_ptr(), la.stdin_data,
                                                    la.share_ptr());
            if (!sr.spawned) {
              throw CulebraError("ProcessError",
                  std::format("Proc.spawn: {} failed: {}.", sr.err_what,
                              std::system_category().message(sr.err_no)),
                  line, col);
            }
            return make_proc_handle(sr.pid, sr.out_fd, sr.err_fd);
          },
          "Object"sv)),
      false);

  return Value(std::move(ns));
}

// --- JSON stringify/parse ---

// Serialize `v`. With `indent > 0` pretty-prints with `indent` spaces
// per nesting level (and a `": "` separator); 0 emits the compact form.
// `sort_keys` walks Object keys alphabetically instead of insertion
// order (for deterministic diff / hash friendliness). `depth` tracks
// recursion for indentation only.
// JSON logic lives in json.h (culebra::json), shared with the JIT/AOT
// runtime in stdlib_jit.h so values, error texts, and positions stay
// byte-identical across backends. These policies adapt the neutral core
// to interp Values; the json_* entry points keep their original names.

struct _JsonValueBuilder {
  using Value = culebra::Value;
  using Object = ObjectValue;
  using Array = ArrayValue;
  static Value make_null() { return Value(); }
  static Value boolean(bool b) { return Value(b); }
  static Value integer(int64_t n) { return Value(static_cast<int64_t>(n)); }
  static Value real(double d) { return Value(d); }
  static Value string(std::string&& s) { return Value(std::move(s)); }
  static Object object_new() { return ObjectValue(); }
  // OrderedSymbolMap owns keys (post-K) so a transient std::string is
  // safe — its bytes are copied into the map's stable index.
  static void object_set(Object& o, const std::string& key, Value&& v) {
    o.initialize(std::string_view(key), v, false);
  }
  static Value object_done(Object&& o) { return Value(std::move(o)); }
  static Array array_new() { return ArrayValue(); }
  static void array_push(Array& a, Value&& v) {
    a.values->push_back(std::move(v));
  }
  static Value array_done(Array&& a) { return Value(std::move(a)); }
  // Value-type storage: C++ stack unwinding releases partials, so the
  // throw-path hooks are no-ops.
  static void object_abandon(Object&) {}
  static void array_abandon(Array&) {}
  static void abandon_value(Value&) {}
};

struct _JsonValueReader {
  using Value = culebra::Value;
  static json::Kind kind(const Value& v) {
    switch (v.type) {
      case Value::Nil:    return json::Kind::Nil;
      case Value::Bool:   return json::Kind::Bool;
      case Value::Long:   return json::Kind::Long;
      case Value::Float:  return json::Kind::Float;
      case Value::String: return json::Kind::String;
      case Value::Array:
      case Value::Tuple:
      case Value::Set:    return json::Kind::Seq;
      case Value::Object: return json::Kind::Object;
      default:            return json::Kind::Other;
    }
  }
  static bool as_bool(const Value& v) { return v.get<bool>(); }
  static int64_t as_long(const Value& v) { return v.get<int64_t>(); }
  static double as_double(const Value& v) { return v.get<double>(); }
  static std::string_view as_string(const Value& v) {
    return v.get<std::string>();
  }
  static const std::vector<Value>& _seq(const Value& v) {
    if (v.type == Value::Array) return *v.to_array().values;
    if (v.type == Value::Tuple) return *v.get<TupleValue>().elements;
    return *v.get<SetValue>().members;
  }
  static size_t seq_size(const Value& v) { return _seq(v).size(); }
  static const Value& seq_at(const Value& v, size_t i) { return _seq(v)[i]; }
  static bool object_has_non_string_keys(const Value& v) {
    const auto& obj = v.to_object();
    return obj.non_string_props && !obj.non_string_props->empty();
  }
  static std::vector<std::pair<std::string_view, const Value*>>
  object_entries(const Value& v) {
    const auto& obj = v.to_object();
    std::vector<std::pair<std::string_view, const Value*>> entries;
    entries.reserve(obj.properties->size());
    for (const auto& [k, sym] : *obj.properties) {
      entries.emplace_back(k, &sym.val);
    }
    return entries;
  }
  static std::string_view type_name(const Value& v) { return v.type_name(); }
};

inline std::string json_stringify(const Value& v, int indent = 0,
                                   bool sort_keys = false) {
  return json::stringify<_JsonValueReader>(v, indent, sort_keys);
}

inline std::string json_stringify_lines(const Value& v,
                                        bool sort_keys = false) {
  return json::stringify_lines<_JsonValueReader>(v, sort_keys);
}

inline Value json_parse(std::string_view s, std::string_view number_mode,
                        bool jsonc) {
  return json::parse<_JsonValueBuilder>(s, number_mode, jsonc);
}

inline Value json_parse_lines(std::string_view s,
                               std::string_view number_mode = "auto",
                               bool jsonc = false) {
  return json::parse_lines<_JsonValueBuilder>(s, number_mode, jsonc);
}

inline Value make_json_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  // `JSON.stringify(v, indent=0, sort_keys=false, lines=false)`:
  //   * indent > 0 pretty-prints; indent == 0 is compact.
  //   * sort_keys walks Object keys alphabetically (deterministic
  //     output for diffs / hashing).
  //   * lines emits JSON Lines from an Array/Tuple/Set — each element
  //     compact on its own line. Incompatible with indent > 0.
  ns.initialize(
      "stringify",
      Value(FunctionValue(
          {
              {"v", false},
              {"indent", false, "Long"sv, nullptr, kw_default_zero()},
              {"sort_keys", false, "Bool"sv, nullptr, kw_default_false()},
              {"lines", false, "Bool"sv, nullptr, kw_default_false()},
          },
          [](std::shared_ptr<Environment> env) {
            const auto& v = env->get("v");
            // indent/sort_keys/lines carry type annotations, so the typed-param
            // binder already rejected non-Long / non-Bool with the shared
            // `parameter '<name>' expects <Type>` message (matching the JIT).
            int indent = static_cast<int>(env->get("indent").get<int64_t>());
            bool sort_keys = env->get("sort_keys").to_bool();
            bool lines = env->get("lines").to_bool();
            if (lines) {
              if (indent > 0) {
                throw CulebraError("TypeError",
                    "JSON.stringify: lines=true is incompatible with indent>0");
              }
              return Value(json_stringify_lines(v, sort_keys));
            }
            return Value(json_stringify(v, indent, sort_keys));
          }, "String"sv)),
      false);
  // `JSON.parse(s, lines=false, number_mode='auto', jsonc=false)`:
  //   * lines splits on `\n` and returns an Array, one entry per line.
  //   * number_mode='float' forces every number to Float (round-trip
  //     friendly when the producer treats numbers uniformly).
  //   * jsonc tolerates `//` / `/* */` comments and trailing commas, so
  //     existing JSONC config files (tsconfig.json, VSCode settings) parse.
  // `number_mode` default is "auto" — no shared canonical for
  // strings since user code could compare by identity in theory.
  static const auto json_number_mode_auto =
      std::make_shared<Value>(Value(std::string("auto")));
  ns.initialize(
      "parse",
      Value(FunctionValue(
          {
              {"s", false, "String"sv},
              {"lines", false, "Bool"sv, nullptr, kw_default_false()},
              {"number_mode", false, "String"sv, nullptr,
               json_number_mode_auto},
              {"jsonc", false, "Bool"sv, nullptr, kw_default_false()},
          },
          [](std::shared_ptr<Environment> env) {
            const auto& s = env->get("s").to_string();
            bool lines = env->get("lines").to_bool();
            const auto& mode = env->get("number_mode").to_string();
            bool jsonc = env->get("jsonc").to_bool();
            if (mode != "auto" && mode != "float") {
              throw CulebraError("ValueError",
                  "JSON.parse: number_mode must be 'auto' or 'float'");
            }
            if (lines) return json_parse_lines(s, mode, jsonc);
            return json_parse(s, mode, jsonc);
          })),
      false);
  return Value(std::move(ns));
}

//===--------------------------------------------------------------------===//
// Regex — a stateless namespace (Regex.find(pat, s) etc.). Exposed as a
// namespace rather than a compiled-handle object so JIT routes calls through
// kNsMethods, avoiding the builtin-method-name collision (find/split/match)
// that breaks class-instance methods in the JIT. A compiled-Regex cache keyed
// by pattern gives reuse. A Match is a data object
// { value, start, end, groups: [Group|nil], named: {name: Group} }; no-match
// is nil. Flags are inline ((?i)/(?m)/(?s)).
//===--------------------------------------------------------------------===//

// A capture group -> { value, start, end }, or nil if it did not participate.
// `offset` shifts the byte spans to absolute positions (find_from searches a
// suffix and reports relative offsets); the new engine's match views are
// immutable, so the shift is applied here at build time rather than by mutating.
inline Value regex_group_value(const reg::Match& c, size_t offset = 0) {
  if (!c.matched()) return Value();
  ObjectValue g;
  g.initialize("value", Value(std::string(c.str())), false);
  g.initialize("start", Value(static_cast<int64_t>(c.begin() + offset)), false);
  g.initialize("end", Value(static_cast<int64_t>(c.end() + offset)), false);
  return Value(std::move(g));
}

// A match -> { value, start, end, groups: [Group|nil], named: {name: Group} }.
// Subscripts access captures directly: `m[i]` -> positional group value,
// `m["name"]` -> named group value (nil when absent / unmatched). The dot
// fields stay for spans (`m.groups[i].start`) and the whole match (`m.value`).
// Templated over the match-like type: `reg::MatchResult` (owning, from
// search/match) and `reg::Match` (a borrowed view, yielded by find_all) share
// the same accessor surface. `named` is the Regex's name->index map (the result
// itself does not expose name enumeration); `offset` is the absolute shift.
template <typename MatchT>
inline Value regex_match_value(const MatchT& m,
                               const std::unordered_map<std::string, int>& named,
                               size_t offset = 0) {
  ObjectValue mo;
  mo.is_match = true;
  mo.initialize("value", Value(std::string(m.str())), false);
  mo.initialize("start", Value(static_cast<int64_t>(m.begin() + offset)), false);
  mo.initialize("end", Value(static_cast<int64_t>(m.end() + offset)), false);
  ArrayValue groups;  // groups[0] is the whole match, then capture groups 1..n
  for (size_t i = 0; i <= m.group_count(); i++)
    groups.values->push_back(regex_group_value(m.group(i), offset));
  mo.initialize("groups", Value(std::move(groups)), false);
  // named: { name -> Group } (data only, so it survives the JIT value model too)
  ObjectValue named_obj;
  for (const auto& kv : named)
    named_obj.initialize(kv.first, regex_group_value(m.group(kv.second), offset),
                         false);
  mo.initialize("named", Value(std::move(named_obj)), false);
  return Value(std::move(mo));
}

// Convert a match-time RegexError (e.g. step-budget exceeded) into a
// CulebraError carrying the call site.
inline void regex_rethrow(const reg::RegexError& e,
                          const std::shared_ptr<Environment>& env) {
  throw CulebraError("RegexError", std::string(e.what()),
                     env->get("__LINE__").to_long(),
                     env->get("__COLUMN__").to_long());
}

// Compile (or cache-hit) a Regex for `pattern`. The Regex namespace functions
// are stateless — the pattern string is the identity — so a small thread-local
// cache gives reuse without a handle. Flags are written inline ((?i)/(?m)/(?s))
// in the pattern, so the cache key is just the pattern.
inline std::shared_ptr<reg::Regex> regex_compile_cached(
    const std::string& pattern, int64_t line, int64_t col) {
  static thread_local std::unordered_map<std::string,
                                          std::shared_ptr<reg::Regex>>
      cache;
  auto it = cache.find(pattern);
  if (it != cache.end()) return it->second;
  std::shared_ptr<reg::Regex> re;
  try {
    re = std::make_shared<reg::Regex>(pattern);
  } catch (const reg::RegexError& e) {
    throw CulebraError("RegexError", std::format("Regex: {}", e.what()), line,
                       col);
  }
  if (cache.size() > 256) cache.clear();  // bound growth
  cache.emplace(pattern, re);
  return re;
}

inline std::shared_ptr<reg::Regex> regex_from_env(
    const std::shared_ptr<Environment>& env) {
  return regex_compile_cached(std::string(env->get("pattern").to_string()),
                              env->get("__LINE__").to_long(),
                              env->get("__COLUMN__").to_long());
}

// `Compress`: gzip (de)compression namespace. The zlib logic is shared with
// the JIT slow-path adapters via compress.h. Binary-safe (to_string_view keeps
// embedded NUL); gunzip raises ValueError on malformed input.
inline Value make_compress_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize(
      "gzip",
      Value(FunctionValue({{"data", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            int64_t line = env->get("__LINE__").to_long();
                            int64_t col = env->get("__COLUMN__").to_long();
                            auto r = culebra::compress::gzip(
                                env->get("data").to_string_view());
                            if (!r.error.empty()) {
                              throw CulebraError("ValueError",
                                                 "Compress.gzip: " + r.error,
                                                 line, col);
                            }
                            return Value(std::move(r.data));
                          },
                          "String"sv)),
      false);
  ns.initialize(
      "gunzip",
      Value(FunctionValue({{"data", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            int64_t line = env->get("__LINE__").to_long();
                            int64_t col = env->get("__COLUMN__").to_long();
                            auto r = culebra::compress::gunzip(
                                env->get("data").to_string_view());
                            if (!r.error.empty()) {
                              throw CulebraError("ValueError",
                                                 "Compress.gunzip: " + r.error,
                                                 line, col);
                            }
                            return Value(std::move(r.data));
                          },
                          "String"sv)),
      false);
  // deflate(data, level=-1): the raw choke behind gzip, minus the gzip
  // wrapper — the zlib (RFC 1950) format Compress.gunzip already auto-detects
  // and decodes, so there is no separate `inflate` to add. `level` follows
  // zlib's own convention (-1 = default, 0 = none, 9 = best); an out-of-range
  // value fails at deflateInit2 and surfaces as the same ValueError shape.
  ns.initialize(
      "deflate",
      Value(FunctionValue(
          {{"data", false, "String"sv},
           {"level", false, "Long"sv, nullptr,
            std::make_shared<Value>(Value(static_cast<int64_t>(-1)))}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            auto level = env->get("level").to_long();
            auto r = culebra::compress::deflate_zlib(
                env->get("data").to_string_view(),
                static_cast<int>(level));
            if (!r.error.empty()) {
              throw CulebraError("ValueError", "Compress.deflate: " + r.error,
                                 line, col);
            }
            return Value(std::move(r.data));
          },
          "String"sv)),
      false);
  return Value(std::move(ns));
}

// `UUID`: generate canonical UUIDs. v4 = random, v7 = time-ordered (Unix-ms
// prefix). Entropy from the shared PRNG (csv-style value-neutral core in
// uuid.h shared with the JIT slow-path adapters).
// `String.from_code_point(cp)` — the inverse of `.code_points()` /
// `.bytes()`: a single Unicode scalar value in, a one-character String out.
// Raises ValueError (via string_from_code_point in shared.h) for values
// above U+10FFFF or in the surrogate range, the same boundary the
// `\u{...}` literal escape enforces at parse time.
inline Value make_string_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize(
      "from_code_point",
      Value(FunctionValue(
          {{"cp", false, "Long"sv}},
          [](std::shared_ptr<Environment> callEnv) {
            return Value(string_from_code_point(callEnv->get("cp").get<int64_t>()));
          },
          "String"sv)),
      false);
  // `String.from_bytes(bytes)` — the inverse of `.bytes()`: raw UTF-8 byte
  // values in (0-255), a String out. No decode validation — culebra Strings
  // tolerate invalid UTF-8, so `String.from_bytes(s.bytes().collect()) == s`
  // holds for every String, including ones with invalid UTF-8 sequences.
  ns.initialize(
      "from_bytes",
      Value(FunctionValue(
          {{"bytes", false, "Array"sv}},
          [](std::shared_ptr<Environment> callEnv) {
            auto line = callEnv->get("__LINE__").to_long();
            auto col = callEnv->get("__COLUMN__").to_long();
            const auto& arr = *callEnv->get("bytes").to_array().values;
            std::string out;
            out.reserve(arr.size());
            for (const auto& v : arr) {
              if (v.type != Value::Long) throw_type_error_at(line, col);
              append_checked_byte(out, v.get<int64_t>());
            }
            return Value(std::move(out));
          },
          "String"sv)),
      false);
  // `String.from_code_points(cps)` — the plural inverse of `.code_points()`:
  // a sequence of Unicode scalar values in, a String out. Each element goes
  // through the same gate as `from_code_point`, so `from_code_points([cp])
  // == from_code_point(cp)`.
  ns.initialize(
      "from_code_points",
      Value(FunctionValue(
          {{"cps", false, "Array"sv}},
          [](std::shared_ptr<Environment> callEnv) {
            auto line = callEnv->get("__LINE__").to_long();
            auto col = callEnv->get("__COLUMN__").to_long();
            const auto& arr = *callEnv->get("cps").to_array().values;
            std::string out;
            out.reserve(arr.size());
            for (const auto& v : arr) {
              if (v.type != Value::Long) throw_type_error_at(line, col);
              append_checked_code_point(out, v.get<int64_t>());
            }
            return Value(std::move(out));
          },
          "String"sv)),
      false);
  return Value(std::move(ns));
}

inline Value make_uuid_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize("v4",
                Value(FunctionValue({}, [](std::shared_ptr<Environment>) {
                                      return Value(culebra::uuid::v4());
                                    },
                                    "String"sv)),
                false);
  ns.initialize("v7",
                Value(FunctionValue({}, [](std::shared_ptr<Environment>) {
                                      return Value(culebra::uuid::v7());
                                    },
                                    "String"sv)),
                false);
  return Value(std::move(ns));
}

// Default `delimiter:` for CSV (comma). A `","` String default the JIT mirrors
// through _jit_default_from_value (the calling-convention single source).
inline const std::shared_ptr<Value>& kw_default_comma() {
  static const auto v = std::make_shared<Value>(Value(std::string(",")));
  return v;
}

// First byte of a `delimiter:` String, or ',' when empty — CSV delimiters are
// a single byte. Shared shape with the JIT adapter's extraction.
inline char csv_delim_char(const Value& v) {
  auto s = v.to_string_view();
  return s.empty() ? ',' : s[0];
}

// Map a coerced CSV cell to a Value (interp side of the neutral result).
inline Value csv_coerced_to_value(const culebra::csv::CoercedCell& cc) {
  using CT = culebra::csv::ColType;
  switch (cc.kind) {
    case CT::String: return Value(std::string(cc.str_val));
    case CT::Long:   return Value(static_cast<int64_t>(cc.long_val));
    case CT::Float:  return Value(cc.float_val);
    case CT::Bool:   return Value(cc.bool_val);
  }
  return Value(std::string(cc.str_val));
}

// Build the CSV.parse result. Without `header`: rows of String fields (Array of
// Array). With `header`: the first row names the columns and each later row
// becomes an Object; `types` (a column-name → type-name Object) coerces the
// named columns, others stay String. Errors carry the 1-based record number +
// column name. Shared shape with the JIT adapter (csv.h single-sources the
// header/types validation and coercion).
inline Value csv_rows_to_value(std::vector<std::vector<std::string>>&& rows,
                               bool header, const Value& typesv, int64_t line,
                               int64_t col) {
  namespace ccsv = culebra::csv;
  bool has_types = typesv.type != Value::Nil;
  if (has_types && !header) {
    throw CulebraError("TypeError", "CSV.parse: types requires header: true",
                       line, col);
  }
  if (!header) {
    ArrayValue out;
    for (auto& row : rows) {
      ArrayValue r;
      for (auto& f : row) r.values->push_back(Value(std::move(f)));
      out.values->push_back(Value(std::move(r)));
    }
    return Value(std::move(out));
  }
  ArrayValue out;
  if (rows.empty()) return Value(std::move(out));  // no header row → empty
  const auto& head = rows[0];
  std::vector<std::pair<std::string, std::string>> tpairs;
  if (has_types) {
    if (typesv.type != Value::Object) {
      throw CulebraError("TypeError",
          "CSV.parse: types must be an Object of String", line, col);
    }
    for (const auto& [k, sym] : *typesv.to_object().properties) {
      if (sym.val.type != Value::String && sym.val.type != Value::StringView) {
        throw CulebraError("TypeError", "CSV.parse: type values must be String",
                           line, col);
      }
      tpairs.emplace_back(std::string(k), std::string(sym.val.to_string_view()));
    }
  }
  std::vector<ccsv::ColType> cts;
  std::string err;
  if (!ccsv::resolve_col_types(head, tpairs, cts, err)) {
    throw CulebraError("ValueError", "CSV.parse: " + err, line, col);
  }
  for (size_t r = 1; r < rows.size(); r++) {
    const auto& row = rows[r];
    if (row.size() != head.size()) {
      throw CulebraError("ValueError",
          std::format("CSV.parse: row {} has {} fields, but the header has {}",
                      r + 1, row.size(), head.size()), line, col);
    }
    ObjectValue obj;
    for (size_t c = 0; c < head.size(); c++) {
      ccsv::CoercedCell cc;
      if (!ccsv::coerce_cell(row[c], cts[c], cc)) {
        throw CulebraError("ValueError",
            std::format("CSV.parse: row {}, column '{}': expected {}, got '{}'",
                        r + 1, head[c], ccsv::col_type_name(cts[c]), row[c]),
            line, col);
      }
      obj.initialize(head[c], csv_coerced_to_value(cc), false);
    }
    out.values->push_back(Value(std::move(obj)));
  }
  return Value(std::move(out));
}

// `CSV`: parse / stringify RFC 4180-ish CSV. The parse/serialize logic is
// shared with the JIT slow-path adapters via csv.h. `parse` returns rows of
// String fields; `stringify` takes an Array of rows (each an Array) and
// renders each field via the same conversion as `to_string` (so numbers and
// other scalars serialize naturally), quoting where needed. The `delimiter:`
// option (default `,`) selects the field separator (e.g. `"\t"` for TSV).
inline Value make_csv_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize(
      "parse",
      Value(FunctionValue({{"text", false, "String"sv},
                           {"delimiter", false, "String"sv, nullptr,
                            kw_default_comma()},
                           {"header", false, "Bool"sv, nullptr,
                            kw_default_false()},
                           {"types", false, ""sv, nullptr, kw_default_nil()}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            int64_t line = env->get("__LINE__").to_long();
                            int64_t col = env->get("__COLUMN__").to_long();
                            auto rows = culebra::csv::parse(
                                env->get("text").to_string_view(),
                                csv_delim_char(env->get("delimiter")));
                            bool header = env->get("header").to_bool();
                            const Value& typesv = env->get("types");
                            return csv_rows_to_value(std::move(rows), header,
                                                     typesv, line, col);
                          },
                          "Array"sv)),
      false);
  ns.initialize(
      "stringify",
      Value(FunctionValue({{"rows", false, "Array"sv},
                           {"delimiter", false, "String"sv, nullptr,
                            kw_default_comma()}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            int64_t line = env->get("__LINE__").to_long();
                            int64_t col = env->get("__COLUMN__").to_long();
                            const auto& rows = env->get("rows").to_array();
                            std::vector<std::vector<std::string>> grid;
                            for (const auto& rv : *rows.values) {
                              if (rv.type != Value::Array) throw_type_error_at(line, col);
                              std::vector<std::string> r;
                              for (const auto& f : *rv.to_array().values)
                                r.push_back(str_display_with_special(f));
                              grid.push_back(std::move(r));
                            }
                            return Value(culebra::csv::stringify(
                                grid, csv_delim_char(env->get("delimiter"))));
                          },
                          "String"sv)),
      false);
  return Value(std::move(ns));
}

// Convert a neutral toml::Node tree into a culebra Value. Tables become
// Objects (insertion order preserved), arrays become Arrays, and scalars map
// to Long/Float/Bool/String. Date-times arrive as raw Strings (decision: no
// Instant type), so they round-trip as ordinary strings.
inline Value toml_node_to_value(const culebra::toml::Node& n) {
  using K = culebra::toml::Kind;
  switch (n.kind) {
    case K::String: return Value(std::string(n.s));
    case K::Int:    return Value(static_cast<int64_t>(n.i));
    case K::Float:  return Value(n.f);
    case K::Bool:   return Value(n.b);
    case K::Array: {
      ArrayValue arr;
      for (const auto& e : n.elems)
        arr.values->push_back(toml_node_to_value(e));
      return Value(std::move(arr));
    }
    case K::Table: {
      ObjectValue obj;
      for (const auto& kv : n.items)
        obj.initialize(std::string_view(kv.first), toml_node_to_value(kv.second),
                       false);
      return Value(std::move(obj));
    }
  }
  return Value();
}

// Convert a culebra Value into a neutral toml::Node for serialization. Objects
// become tables (non-String keys rejected, mirroring JSON), Array/Tuple/Set
// become arrays, scalars map across; everything else is a TypeError.
inline culebra::toml::Node value_to_toml_node(const Value& v, int64_t line,
                                              int64_t col) {
  using N = culebra::toml::Node;
  switch (v.type) {
    case Value::Bool:   return N::boolean(v.get<bool>());
    case Value::Long:   return N::integer(v.get<int64_t>());
    case Value::Float:  return N::floating(v.get<double>());
    case Value::String: return N::string(v.get<std::string>());
    case Value::Array:
    case Value::Tuple:
    case Value::Set: {
      const std::vector<Value>* xs = nullptr;
      if (v.type == Value::Array) xs = v.to_array().values.get();
      else if (v.type == Value::Tuple) xs = v.template get<TupleValue>().elements.get();
      else xs = v.template get<SetValue>().members.get();
      N arr = N::array();
      for (const auto& e : *xs) arr.elems.push_back(value_to_toml_node(e, line, col));
      return arr;
    }
    case Value::Object: {
      const auto& obj = v.to_object();
      if (obj.non_string_props && !obj.non_string_props->empty()) {
        throw CulebraError("TypeError",
            "TOML.stringify: Object has non-String keys", line, col);
      }
      N tbl = N::table();
      for (const auto& [k, sym] : *obj.properties)
        tbl.set(std::string(k), value_to_toml_node(sym.val, line, col));
      return tbl;
    }
    default:
      throw CulebraError("TypeError", std::format(
          "TOML.stringify: cannot serialize {}", v.type_name()), line, col);
  }
}

// `TOML`: parse / stringify TOML text. The grammar and serialization live in
// the value-neutral toml.h core, shared with the JIT runtime helpers so the
// three backends agree byte-for-byte. `parse` returns an Object; `stringify`
// takes an Object (TOML documents are tables) and renders it, expanding
// sub-tables into `[section]` headers. Date-times round-trip as Strings.
inline Value make_toml_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize(
      "parse",
      Value(FunctionValue({{"text", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            try {
                              auto root = culebra::toml::parse(
                                  env->get("text").to_string_view());
                              return toml_node_to_value(root);
                            } catch (const culebra::toml::ParseError& e) {
                              throw CulebraError("ValueError",
                                  std::format("TOML.parse: {}", e.message),
                                  e.line, e.col);
                            }
                          },
                          "Object"sv)),
      false);
  ns.initialize(
      "stringify",
      Value(FunctionValue({{"v", false, "Object"sv},
                           {"sort_keys", false, "Bool"sv, nullptr,
                            kw_default_false()}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            int64_t line = env->get("__LINE__").to_long();
                            int64_t col = env->get("__COLUMN__").to_long();
                            bool sort_keys = env->get("sort_keys").to_bool();
                            auto root = value_to_toml_node(env->get("v"), line,
                                                           col);
                            return Value(culebra::toml::stringify(root,
                                                                 sort_keys));
                          },
                          "String"sv)),
      false);
  return Value(std::move(ns));
}

// Default `path:` for Env.load (".env"). A String default the JIT mirrors
// through _jit_default_from_value (the calling-convention single source).
inline const std::shared_ptr<Value>& kw_default_dotenv() {
  static const auto v = std::make_shared<Value>(Value(std::string(".env")));
  return v;
}

// Build an Object from parsed dotenv pairs (immutable fields, like JSON.parse).
inline Value _env_pairs_to_object(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  ObjectValue o;
  for (const auto& [k, v] : pairs)
    o.initialize(std::string_view(k), Value(std::string(v)), false);
  return Value(std::move(o));
}

// `Env`: dotenv-style `.env` parsing + loading. The parse logic is shared with
// the JIT slow-path adapters via env.h. `parse` is pure; `load` reads a file,
// sets each entry into the process environment (overwriting only when
// `override: true`), then returns the parsed Object. Both return an Object of
// String values keyed by name.
inline Value make_env_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize(
      "parse",
      Value(FunctionValue({{"text", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            return _env_pairs_to_object(culebra::env::parse(
                                env->get("text").to_string_view()));
                          },
                          "Object"sv)),
      false);
  ns.initialize(
      "load",
      Value(FunctionValue(
          {{"path", false, "String"sv, nullptr, kw_default_dotenv()},
           {"override", false, "Bool"sv, nullptr, kw_default_false()}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->get("__LINE__").to_long();
            int64_t col = env->get("__COLUMN__").to_long();
            const auto& path = env->get("path").to_string();
            bool overwrite = env->get("override").to_bool();
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs)
              _io_throw(std::format("Env.load: cannot open '{}'", path),
                        line, col);
            std::string content((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
            auto pairs = culebra::env::parse(content);
            for (const auto& [k, v] : pairs)
              os_setenv(k.c_str(), v.c_str(), overwrite ? 1 : 0);
            return _env_pairs_to_object(pairs);
          },
          "Object"sv)),
      false);
  return Value(std::move(ns));
}

// `Hash`: message digests + HMAC, returning the lowercase hex digest. The
// crypto is self-hosted in hash.h and shared with the JIT slow-path adapters,
// so all three backends produce identical digests. Flat namespace, no kwargs.
inline Value make_hash_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  // digest(name): one String -> hex String.
  auto digest = [&](const char* name, std::string (*fn)(std::string_view)) {
    ns.initialize(
        name,
        Value(FunctionValue({{"data", false, "String"sv}},
                            [fn](std::shared_ptr<Environment> env) -> Value {
                              return Value(fn(env->get("data").to_string_view()));
                            },
                            "String"sv)),
        false);
  };
  digest("sha256", culebra::hashing::sha256);
  digest("sha1", culebra::hashing::sha1);
  digest("sha512", culebra::hashing::sha512);
  digest("md5", culebra::hashing::md5);
  // hmac(name): (key, msg) Strings -> hex String.
  auto hmac = [&](const char* name,
                  std::string (*fn)(std::string_view, std::string_view)) {
    ns.initialize(
        name,
        Value(FunctionValue({{"key", false, "String"sv},
                             {"data", false, "String"sv}},
                            [fn](std::shared_ptr<Environment> env) -> Value {
                              return Value(fn(env->get("key").to_string_view(),
                                              env->get("data").to_string_view()));
                            },
                            "String"sv)),
        false);
  };
  hmac("hmac_sha256", culebra::hashing::hmac_sha256);
  hmac("hmac_sha1", culebra::hashing::hmac_sha1);
  hmac("hmac_sha512", culebra::hashing::hmac_sha512);
  return Value(std::move(ns));
}

// `Encoding`: text codec namespace. Grouped into sub-namespaces by scheme
// (`Encoding.html` / `base64` / `hex` / `url`). The codec logic is shared
// with the JIT slow-path adapters via shared.h.
inline Value make_encoding_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  ObjectValue html;
  html.initialize(
      "escape",
      Value(FunctionValue({{"s", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            return Value(culebra::html_escape(
                                env->get("s").to_string_view()));
                          },
                          "String"sv)),
      false);
  html.initialize(
      "unescape",
      Value(FunctionValue({{"s", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            return Value(culebra::html_unescape(
                                env->get("s").to_string_view()));
                          },
                          "String"sv)),
      false);
  ns.initialize("html", Value(std::move(html)), false);

  ObjectValue base64;
  base64.initialize(
      "encode",
      Value(FunctionValue({{"s", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            return Value(culebra::base64_encode(
                                env->get("s").to_string_view()));
                          },
                          "String"sv)),
      false);
  base64.initialize(
      "decode",
      Value(FunctionValue({{"s", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            int64_t line = env->get("__LINE__").to_long();
                            int64_t col = env->get("__COLUMN__").to_long();
                            auto r = culebra::base64_decode(
                                env->get("s").to_string_view());
                            if (!r) {
                              throw CulebraError(
                                  "ValueError",
                                  "Encoding.base64.decode: invalid base64", line,
                                  col);
                            }
                            return Value(std::move(*r));
                          },
                          "String"sv)),
      false);
  ns.initialize("base64", Value(std::move(base64)), false);

  ObjectValue hex;
  hex.initialize(
      "encode",
      Value(FunctionValue({{"s", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            return Value(culebra::hex_encode(
                                env->get("s").to_string_view()));
                          },
                          "String"sv)),
      false);
  hex.initialize(
      "decode",
      Value(FunctionValue({{"s", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            int64_t line = env->get("__LINE__").to_long();
                            int64_t col = env->get("__COLUMN__").to_long();
                            auto r = culebra::hex_decode(
                                env->get("s").to_string_view());
                            if (!r) {
                              throw CulebraError(
                                  "ValueError",
                                  "Encoding.hex.decode: invalid hex", line, col);
                            }
                            return Value(std::move(*r));
                          },
                          "String"sv)),
      false);
  ns.initialize("hex", Value(std::move(hex)), false);

  ObjectValue url;
  url.initialize(
      "encode",
      Value(FunctionValue({{"s", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            return Value(culebra::url_encode(
                                env->get("s").to_string_view()));
                          },
                          "String"sv)),
      false);
  url.initialize(
      "decode",
      Value(FunctionValue({{"s", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) -> Value {
                            return Value(culebra::url_decode(
                                env->get("s").to_string_view()));
                          },
                          "String"sv)),
      false);
  ns.initialize("url", Value(std::move(url)), false);

  return Value(std::move(ns));
}

// `_Regex`: low-level stateless primitives over the engine. The user-facing
// object API `Regex.compile(pat).find(s)` is the culebra-source `Regex` class
// in REGEX_MODULE_SOURCE, which delegates here — the `_Time` / `Time` split.
// All primitives take (pattern, subject, ...); flags are folded into the
// pattern inline ((?i)/(?m)/(?s)) by the wrapper. A Match is a data object
// { value, start, end, groups: [Group|nil], named: {name: Group} }; no-match nil.
inline Value make_regex_primitives_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  // Subject + pattern accept either string flavor (String / StringView), so
  // results of String.split / .slice (which return StringView) compose with
  // Regex methods. Mirrors the StringLike-typed String methods.
  const std::vector<FunctionValue::Parameter> ps = {{"pattern", false, "StringLike"sv},
                                                    {"s", false, "StringLike"sv}};

  // _Regex.check(pattern) -> Nil  (validate eagerly at Regex.compile time)
  ns.initialize("check",
                Value(FunctionValue(
                    {{"pattern", false, "StringLike"sv}},
                    [](std::shared_ptr<Environment> env) -> Value {
                      regex_compile_cached(
                          std::string(env->get("pattern").to_string()),
                          env->get("__LINE__").to_long(),
                          env->get("__COLUMN__").to_long());
                      return Value();
                    })),
                false);

  ns.initialize("test",
                Value(FunctionValue(
                    ps,
                    [](std::shared_ptr<Environment> env) -> Value {
                      auto re = regex_from_env(env);
                      std::string s{env->get("s").to_string()};
                      try {
                        return Value(re->test(s));
                      } catch (const reg::RegexError& e) {
                        regex_rethrow(e, env);
                        return Value();
                      }
                    },
                    "Bool"sv)),
                false);

  auto search_fn = [ps](bool anchored) {
    return Value(FunctionValue(
        ps, [anchored](std::shared_ptr<Environment> env) -> Value {
          auto re = regex_from_env(env);
          std::string s{env->get("s").to_string()};
          try {
            auto m = anchored ? re->match(s) : re->search(s);
            return m.matched() ? regex_match_value(m, re->named_groups())
                               : Value();
          } catch (const reg::RegexError& e) {
            regex_rethrow(e, env);
            return Value();
          }
        }));
  };
  ns.initialize("find", search_fn(false), false);
  ns.initialize("match", search_fn(true), false);

  // find_from(pattern, s, pos) -> { m: Match|nil, next: Int }. Drives the
  // lazy `Regex.find_iter` generator: one stateless engine scan step —
  // find_at gives the leftmost match at/after byte `pos` (absolute offsets)
  // and the resume position (one grapheme past an empty match), and lets
  // anchors see the full subject rather than a suffix.
  ns.initialize(
      "find_from",
      Value(FunctionValue(
          {{"pattern", false, "StringLike"sv},
           {"s", false, "StringLike"sv},
           {"pos", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            auto re = regex_from_env(env);
            std::string s{env->get("s").to_string()};
            int64_t pos = env->get("pos").to_long();
            ObjectValue out;
            try {
              auto r = re->find_at(s, pos < 0 ? s.size() + 1
                                              : static_cast<size_t>(pos));
              out.initialize("m",
                             r.m.matched()
                                 ? regex_match_value(r.m, re->named_groups())
                                 : Value(),
                             false);
              out.initialize("nxt", Value(static_cast<int64_t>(r.next_pos)),
                             false);
              return Value(std::move(out));
            } catch (const reg::RegexError& e) {
              regex_rethrow(e, env);
              return Value();
            }
          })),
      false);

  ns.initialize("find_all",
                Value(FunctionValue(
                    ps,
                    [](std::shared_ptr<Environment> env) -> Value {
                      auto re = regex_from_env(env);
                      std::string s{env->get("s").to_string()};
                      ArrayValue av;
                      try {
                        for (auto m : re->find_all(s))
                          av.values->push_back(
                              regex_match_value(m, re->named_groups()));
                      } catch (const reg::RegexError& e) {
                        regex_rethrow(e, env);
                      }
                      return Value(std::move(av));
                    })),
                false);

  // find_all_str(pattern, s) -> [String]: just the matched texts, skipping the
  // per-match Match object (groups/named construction dominated match-dense
  // workloads — ~3x faster than find_all when you only need the texts).
  ns.initialize("find_all_str",
                Value(FunctionValue(
                    ps,
                    [](std::shared_ptr<Environment> env) -> Value {
                      auto re = regex_from_env(env);
                      std::string s{env->get("s").to_string()};
                      ArrayValue av;
                      try {
                        for (auto m : re->find_all(s))
                          av.values->push_back(Value(std::string(m.str())));
                      } catch (const reg::RegexError& e) {
                        regex_rethrow(e, env);
                      }
                      return Value(std::move(av));
                    })),
                false);

  // find_all_index(pattern, s) -> [Int]: a flat array of byte spans
  // [s0, e0, s1, e1, ...] — positions only, no strings. Longs are stored
  // inline in the Array (no per-element heap object), so the whole result is
  // one allocation regardless of match count.
  ns.initialize("find_all_index",
                Value(FunctionValue(
                    ps,
                    [](std::shared_ptr<Environment> env) -> Value {
                      auto re = regex_from_env(env);
                      std::string s{env->get("s").to_string()};
                      ArrayValue av;
                      try {
                        for (auto m : re->find_all(s)) {
                          av.values->push_back(
                              Value(static_cast<int64_t>(m.begin())));
                          av.values->push_back(
                              Value(static_cast<int64_t>(m.end())));
                        }
                      } catch (const reg::RegexError& e) {
                        regex_rethrow(e, env);
                      }
                      return Value(std::move(av));
                    })),
                false);

  // count(pattern, s) -> Long: number of non-overlapping matches, no objects.
  ns.initialize("count",
                Value(FunctionValue(
                    ps,
                    [](std::shared_ptr<Environment> env) -> Value {
                      auto re = regex_from_env(env);
                      std::string s{env->get("s").to_string()};
                      try {
                        return Value(static_cast<int64_t>(re->find_all(s).size()));
                      } catch (const reg::RegexError& e) {
                        regex_rethrow(e, env);
                        return Value();
                      }
                    },
                    "Long"sv)),
                false);

  ns.initialize("replace_all",
                Value(FunctionValue(
                    {{"pattern", false, "StringLike"sv},
                     {"s", false, "StringLike"sv},
                     {"repl", false, "StringLike"sv}},
                    [](std::shared_ptr<Environment> env) -> Value {
                      auto re = regex_from_env(env);
                      std::string s{env->get("s").to_string()};
                      std::string repl{env->get("repl").to_string()};
                      try {
                        return Value(re->replace_all(s, repl));
                      } catch (const reg::RegexError& e) {
                        regex_rethrow(e, env);
                        return Value();
                      }
                    },
                    "String"sv)),
                false);

  // replace_first(pattern, s, repl) -> String: like replace_all but only the
  // leftmost match. Same $-template grammar (regexlib::replace_first).
  ns.initialize("replace_first",
                Value(FunctionValue(
                    {{"pattern", false, "StringLike"sv},
                     {"s", false, "StringLike"sv},
                     {"repl", false, "StringLike"sv}},
                    [](std::shared_ptr<Environment> env) -> Value {
                      auto re = regex_from_env(env);
                      std::string s{env->get("s").to_string()};
                      std::string repl{env->get("repl").to_string()};
                      try {
                        return Value(re->replace_first(s, repl));
                      } catch (const reg::RegexError& e) {
                        regex_rethrow(e, env);
                        return Value();
                      }
                    },
                    "String"sv)),
                false);

  // split: slice the subject between successive matches (find_all + substr).
  ns.initialize("split",
                Value(FunctionValue(
                    ps,
                    [](std::shared_ptr<Environment> env) -> Value {
                      auto re = regex_from_env(env);
                      std::string s{env->get("s").to_string()};
                      ArrayValue av;
                      try {
                        size_t cursor = 0;
                        for (auto m : re->find_all(s)) {
                          av.values->push_back(
                              Value(s.substr(cursor, m.begin() - cursor)));
                          cursor = m.end();
                        }
                        av.values->push_back(Value(s.substr(cursor)));
                      } catch (const reg::RegexError& e) {
                        regex_rethrow(e, env);
                      }
                      return Value(std::move(av));
                    })),
                false);

  return Value(std::move(ns));
}

// Defined in isolate.h (included at the end of this header). Forward-declared
// so setup_built_in_functions can register the `Isolate`/`Channel` namespaces;
// the bodies pull in the isolate/sendable machinery which itself depends on the
// full stdlib (environment(), Interpreter), hence the bottom include.
inline Value make_isolate_namespace();
inline Value make_channel_namespace();
inline Value make_shared_namespace();
inline Value make_signal_namespace();
inline Value make_parallel_namespace();

// Effects abort signal (spike): a distinct C++ type so `eval_try` (which
// catches Value / CulebraError / runtime_error only) passes it through —
// defers still run via its catch-all rethrow — and `__eff_catch_abort` at
// the handle driver is the only catcher. Interp twin of CulebraEffAbort.
struct InterpEffAbort {
  Value val;
};

inline void setup_built_in_functions(Environment& env) {
  using namespace std::literals;

  env.initialize(
      "to_long",
      Value(FunctionValue({{"v", false}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& v = env->get("v");
                            auto line = env->get("__LINE__").to_long();
                            auto col = env->get("__COLUMN__").to_long();
                            if (v.type == Value::Long) return v;
                            if (v.type == Value::Float) {
                              // Truncate toward zero (matches Python's int()).
                              return Value(static_cast<int64_t>(v.get<double>()));
                            }
                            if (v.type != Value::String &&
                                v.type != Value::StringView)
                              throw_type_error_at(line, col);
                            return Value(parse_long_strict(
                                std::string(v.to_string_view()), line, col));
                          },
                          "Long"sv)),
      false);

  env.initialize(
      "to_float",
      Value(FunctionValue({{"v", false}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& v = env->get("v");
                            auto line = env->get("__LINE__").to_long();
                            auto col = env->get("__COLUMN__").to_long();
                            if (v.type == Value::Float) return v;
                            if (v.type == Value::Long) {
                              return Value(static_cast<double>(v.get<int64_t>()));
                            }
                            if (v.type != Value::String &&
                                v.type != Value::StringView)
                              throw_type_error_at(line, col);
                            return Value(parse_double_strict(
                                std::string(v.to_string_view()), line, col));
                          },
                          "Float"sv)),
      false);

  env.initialize("to_string",
                 Value(FunctionValue({{"v", false}},
                                     [](std::shared_ptr<Environment> env) {
                                       return Value(str_display_with_special(
                                           env->get("v")));
                                     },
                                     "String"sv)),
                 false);

  // `hash(v)`: companion to `to_string` for the Hashable trait. Primitives
  // go through ValueHash (same path Object/Set keys use); Object with a
  // user-defined `hash()` method invokes it. Anything else throws.
  env.initialize(
      "hash",
      Value(FunctionValue(
                {{"v", false}},
                [](std::shared_ptr<Environment> env) {
                  const auto& v = env->get("v");
                  if (v.type == Value::Object) {
                    const auto& obj = v.to_object();
                    if (!obj.has("hash")) {
                      throw CulebraError("TypeError",
                          "unhashable type: 'Object' (no hash() method)");
                    }
                    auto r = _invoke_method_no_args(v, "hash");
                    if (r.type != Value::Long) {
                      throw CulebraError("TypeError",
                          "hash() must return Long");
                    }
                    return r;
                  }
                  return Value(static_cast<int64_t>(ValueHash{}(v)));
                },
                "Long"sv)),
      false);

  env.initialize(
      "type_of",
      Value(FunctionValue({{"v", false}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& v = env->get("v");
                            const char* n = "Nil";
                            switch (v.type) {
                              case Value::Nil:      n = "Nil"; break;
                              case Value::Bool:     n = "Bool"; break;
                              case Value::Long:     n = "Long"; break;
                              case Value::Float:    n = "Float"; break;
                              case Value::String:   n = "String"; break;
                              case Value::Array:    n = "Array"; break;
                              case Value::Object:   n = "Object"; break;
                              case Value::Function: n = "Function"; break;
                              case Value::Tensor:   n = "Tensor"; break;
                              case Value::Tuple:    n = "Tuple"; break;
                              case Value::Set:      n = "Set"; break;
                              case Value::StringView: n = "StringView"; break;
                            }
                            return Value(std::string(n));
                          },
                          "String"sv)),
      false);

  // Shallow-copy a class instance / object (the effects continuation clone,
  // `__eff_copy`): a fresh property map — scalars independent, referenced heap
  // values aliased, methods and `class` tag carried over — mirror of `{...obj}`
  // spread. Interp twin of the JIT `culebra_runtime_eff_copy` (which documents
  // why the clone stays native rather than a culebra method).
  env.initialize(
      "__eff_copy",
      Value(FunctionValue(
          {{"v", false}},
          [](std::shared_ptr<Environment> env) {
            const auto& v = env->get("v");
            if (v.type != Value::Object) return v;
            const auto& src = v.to_object();
            ObjectValue copy;
            for (const auto& [k, sym] : *src.properties) {
              copy.initialize(k, sym.val, sym.mut);
            }
            // Share the class meta, like the JIT clone (`o->proto =
            // src->proto`): the copy stays an instance of the same class.
            copy.properties->proto = src.properties->proto;
            return Value(std::move(copy));
          },
          "Object"sv)),
      false);

  // Throw the effects abort signal carrying `v`. See InterpEffAbort: user
  // try/catch passes it through; only __eff_catch_abort stops it.
  env.initialize(
      "__eff_abort",
      Value(FunctionValue(
          {{"v", false}},
          [](std::shared_ptr<Environment> env) -> Value {
            throw InterpEffAbort{env->get("v")};
          })),
      false);

  // Run `fn`, catching only the abort signal. Returns [aborted, value].
  // Any other exception keeps unwinding untouched.
  env.initialize(
      "__eff_catch_abort",
      Value(FunctionValue(
          {{"fn", false, "Function"sv}},
          [](std::shared_ptr<Environment> env) {
            ArrayValue pair;
            try {
              auto r = _invoke_callback(env->get("fn"));
              pair.values->emplace_back(false);
              pair.values->emplace_back(std::move(r));
            } catch (const InterpEffAbort& a) {
              pair.values->emplace_back(true);
              pair.values->emplace_back(a.val);
            }
            return Value(std::move(pair));
          },
          "Array"sv)),
      false);

  // Install a builtin namespace, tagging its ObjectValue so reading an unknown
  // member raises AttributeError instead of silently yielding nil (see
  // ObjectValue::is_namespace). `name` must be a static string — its pointer is
  // kept as `ns_name` for the error message.
  auto ns_init = [&](const char* name, Value v) {
    if (v.type == Value::Object) {
      auto& o = v.to_object();
      o.is_namespace = true;
      o.ns_name = name;
    }
    env.initialize(name, std::move(v), false);
  };

  ns_init("Math", make_math_namespace());
  ns_init("IO", make_io_namespace());
  ns_init("FS", make_fs_namespace());
  ns_init("File", make_file_namespace());
  ns_init("Embed", make_embed_namespace());
  // Wrapped C++ classes (wrap.h declarations, e.g. foreign_binding.h's
  // `__Foreign.Counter`): one namespace object per declared ns, holding
  // one class object (ctor + statics) per class. Registered at
  // static-init time, so the registry is complete before setup runs.
  {
    std::map<std::string, ObjectValue> wrapped_ns;
    // Stable name pointers for ns_name: the registry vector is a program-
    // lifetime static, so its `wc.ns` strings outlive every namespace object.
    std::map<std::string, const char*> stable_ns_name;
    for (const auto& wc : wrapped_classes()) {
      wrapped_ns[wc.ns].initialize(wc.name, wc.build_class_object(), false);
      stable_ns_name.emplace(wc.ns, wc.ns.c_str());
    }
    for (auto& [ns, obj] : wrapped_ns) {
      obj.is_namespace = true;
      obj.ns_name = stable_ns_name[ns];
      env.initialize(ns, Value(std::move(obj)), false);
    }
  }
  ns_init("_Time", make_time_primitives_namespace());
  ns_init("_Term", make_term_primitives_namespace());
  ns_init("_Canvas", make_canvas_primitives_namespace());
  ns_init("Random", make_random_namespace());
  ns_init("Sys", make_sys_namespace());
  ns_init("GC", make_gc_namespace());
  ns_init("Tensor", make_tensor_namespace());
  ns_init("JSON", make_json_namespace());
  ns_init("Encoding", make_encoding_namespace());
  ns_init("Compress", make_compress_namespace());
  ns_init("Hash", make_hash_namespace());
  ns_init("CSV", make_csv_namespace());
#if defined(CULEBRA_SQLITE_ENABLED)
  ns_init("SQLite", make_sqlite_namespace());
#endif
  ns_init("TOML", make_toml_namespace());
  ns_init("Env", make_env_namespace());
  ns_init("UUID", make_uuid_namespace());
  ns_init("String", make_string_namespace());
  ns_init("_Regex", make_regex_primitives_namespace());
  ns_init("Proc", make_proc_namespace());
  ns_init("Net", make_net_namespace());
#if defined(CULEBRA_HTTP_ENABLED)
  ns_init("Http", make_http_namespace());
#endif
  ns_init("Isolate", make_isolate_namespace());
  ns_init("Channel", make_channel_namespace());
  ns_init("Signal", make_signal_namespace());
  ns_init("Parallel", make_parallel_namespace());
  ns_init("SharedBuffer", make_shared_buffer_namespace());
  ns_init("Shared", make_shared_namespace());
}

// Embedded culebra source for stdlib modules that are easier to express
// in culebra than in C++. `environment()` registers each of them lazily;
// the JIT/AOT path registers a builder for it instead (see
// splice_stdlib_preamble).
//
// `Time` — Instant / Duration classes wrapping `_Time` primitives.
// Operator overloads (`__add__` / `__sub__` / `__mul__` / `__div__`
// / `__neg__` / `__lt__` / `__le__` / `__eq__`) give natural
// timestamp arithmetic.
//
// The module bodies live in src/preambles/*.cul (the editable source of
// truth); misc/gen_preambles.sh embeds each as a <NAME>_MODULE_SOURCE
// constant in the generated header below (run `just gen-preambles`; CI
// checks they're in sync). The per-module notes here and below document
// those sources.
#include "stdlib_preambles.gen.h"

// `Term` — terminal-control / TUI primitives layered on the `_Term`
// native helpers (raw mode / size / key read / flush). Provides ANSI
// colour + escape builders (pure strings), a buffered `Screen` for
// flicker-free frames, normalized key names, and `Term.app` — a render
// loop wrapper that enters raw mode + the alternate screen and restores
// the terminal on scope exit (normal return, exception, or Ctrl+C) via
// `defer`. Written as culebra source so it is automatically symmetric
// across interp / JIT / AOT. A C++ raw string so culebra's `\x1b`
// escapes pass through verbatim.

// `Args` — declarative CLI argument parser. Spec is a culebra
// Object listing positionals + options + subcommands; `Args.parse`
// returns an Object with parsed fields, prints help on `--help`,
// and reports errors with `Sys.exit(2)`. `try_parse` raises
// `{kind: \"ArgParseError\", message}` for programmatic control.

// Matcher family — `assert_true` / `assert_false` / `assert_eq` /
// `assert_ne` / `assert_lt` / `assert_le` / `assert_gt` / `assert_ge` /
// `assert_throws` / `assert_close`. Authored in culebra so all 3
// backends (interp/JIT/AOT) share one implementation: the `==` / `<`
// operators here dispatch through `__eq__` / `__lt__` on class
// instances, matching the operator semantics each backend already
// implements. `assert` itself is not in culebra — production code uses
// `if (!cond) throw {kind: "AssertionError", ...}` (Go / Ruby style).

// `Regex` — the user-facing object API over the native `_Regex` primitives
// (the `_Time` / `Time` split). `Regex.compile(pat, flags?)` returns a Regex
// instance whose methods delegate to `_Regex`; flags ("i"/"m"/"s") are folded
// into the pattern as an inline `(?…)` group. Patterns are best written as
// single-quoted raw strings ('\d+'). A Match is a data object
// { value, start, end, groups: [Group|nil], named: {name: Group} }; no-match nil.

// `s.replace(pat, repl)` (UFCS over this free function) — replace every
// occurrence and return a new String, so transforms chain:
//   text.replace(re'\s+', " ").replace("，", "、")...
// `pat` String -> literal replace (split/join); `pat` a compiled Regex ->
// regex replace (so `repl` may be a `$1`/`$<name>` template or a
// `fn (Match) -> String` callback, via the Regex's own `replace_all`).
// All-occurrences semantics match Python's `str.replace`.

// `Log` — leveled, structured logging to stderr, built on `_Time` (timestamp),
// `IO.eprint` (sink), `JSON.stringify` (json format), and `Sys.env`. Levels
// debug<info<warn<error; the threshold ($LOG_LEVEL or set_level, default info)
// filters. `Log.{level}(msg, fields={})` emits `msg` plus the structured
// fields; `Log.with(fields)` returns a child logger that binds those fields
// into every record (request-scoped context). Format ($LOG_FORMAT or
// set_format, default "text", or "json" for JSON Lines); the text level is
// colored when stderr is a tty.

// JIT/AOT only: rewrite a lazy source module so the entry module registers a
// captureless builder thunk instead of materializing a top-level slot.
//
// interp binds these culebra-source modules lazily (initialize_lazy) and
// re-resolves them per environment, so a closure shipped to another isolate is
// rebuilt there from the same source and never carries the module Object's
// native members (which aren't Sendable). The JIT can't carry the built Object
// either, so it drops the public slot and routes every reference — main and
// child — through namespace_get + the builder registry (one instance per
// Runtime). The builder is the whole module wrapped in a thunk
// `fn(){ <module body>; _x_module() }`, which is captureless: it references
// only builtins (resolved per-Runtime via namespace_get) plus its own nested
// helpers/classes, so namespace_get can rebuild + invoke it on any Runtime —
// exactly how run_isolate_child_jit rebuilds a user closure from a shared
// fn_ptr. The trailing public binding `let X = _x_module()` becomes the thunk's
// result expression `_x_module()`.
inline std::string _wrap_lazy_ns_module(std::string_view src, const char* pub,
                                        const char* builder) {
  std::string s(src);
  std::string bind = std::string("let ") + pub + " = " + builder + "()";
  auto pos = s.find(bind);
  // Every namespace module ends with its public binding; a miss means the
  // source shape changed (the JIT would silently lose the module). Assert so a
  // source edit that drops/renames the binding is caught in a dev build.
  assert(pos != std::string::npos &&
         "lazy-ns module is missing its `let X = _x_module()` public binding");
  if (pos != std::string::npos)
    s.replace(pos, bind.size(), std::string(builder) + "()");
  return "_lazy_ns_register(\"" + std::string(pub) + "\", fn(){ " + s + " });";
}

// The culebra-source stdlib modules whose public binding is a namespace
// object: public name, source, and the builder its last statement calls.
// One list for both backends — the interp's lazy bindings and the JIT/AOT
// builder registrations read it — so a module added to one cannot go missing
// from the other.
struct LazyNsModule {
  const char* name;
  const char* source;
  const char* builder;
};

inline std::span<const LazyNsModule> lazy_ns_modules() {
  static constexpr LazyNsModule kModules[] = {
      {"Time", TIME_MODULE_SOURCE, "_time_module"},
      {"Term", TERM_MODULE_SOURCE, "_term_module"},
      {"Canvas", CANVAS_MODULE_SOURCE, "_canvas_module"},
      {"Args", ARGS_MODULE_SOURCE, "_args_module"},
      {"Regex", REGEX_MODULE_SOURCE, "_regex_module"},
      {"Log", LOG_MODULE_SOURCE, "_log_module"},
      {"Path", PATH_MODULE_SOURCE, "_path_module"},
      // Algebraic-effects runtime. The transform has already lowered every
      // effect construct into `__Eff.*` calls by the time we see the AST, so
      // that one token is the exact marker (see effects_transform.h).
      {"__Eff", EFFECTS_MODULE_SOURCE, "_eff_module"},
#ifdef CULEBRA_ENABLE_WEBVIEW
      // `Desktop.run` facade — only when the Webview namespace it drives is
      // built in.
      {"Desktop", DESKTOP_MODULE_SOURCE, "_desktop_module"},
#endif
  };
  return kModules;
}

// The source backing a bare-function group (see LazyFnGroup in shared.h).
inline std::string_view lazy_fn_group_source(std::string_view group) {
  if (group == "__Matchers") return MATCHERS_MODULE_SOURCE;
  if (group == "__Replace") return STRING_REPLACE_MODULE_SOURCE;
  return {};
}

// JIT/AOT only: give a bare-function module the same lazy builder a namespace
// module gets, by appending an Object literal that exposes its functions as
// members. A bare `assert_eq` then resolves to that Object's member — one
// closure per Runtime, reached identically from every module — where the
// interp resolves the same source's `let` into the one global environment.
inline std::string _wrap_lazy_fn_group(const LazyFnGroup& g) {
  // Trimmed because the members literal is appended as a further statement:
  // the `;` separator must not land after the source's trailing newline.
  auto src = trim_ascii(lazy_fn_group_source(g.name));
  // A group declared in shared.h with no source here would register a builder
  // that binds nothing, and every one of its names would raise NameError under
  // the JIT while the interp resolved them fine.
  assert(!src.empty() && "bare-function group has no source module");
  std::string members;
  for (auto m : g.members) {
    if (!members.empty()) members += ", ";
    members += std::string(m) + ": " + std::string(m);
  }
  return "_lazy_ns_register(\"" + std::string(g.name) + "\", fn(){ " +
         std::string(src) + "; {" + members + "} });";
}

// Every token the parsed program carries. Comments and whitespace are gone
// by this point, and a token is a whole lexeme — which is what makes the
// module selection below name-exact instead of substring-based.
inline void collect_ast_tokens(const peg::Ast& ast,
                               std::unordered_set<std::string_view>& out) {
  if (ast.is_token) {
    if (!ast.token.empty()) out.insert(ast.token);
    return;
  }
  for (const auto& n : ast.nodes) collect_ast_tokens(*n, out);
}

// The JIT/AOT stdlib preamble: `_lazy_ns_register("Ns", fn(){...})`
// statements, one per stdlib module the program names. They are pure runtime
// side effects with no scope bindings, so they run once in their own module
// ahead of every other one, and every reference — in any module, from any
// closure — resolves through the builder registry to a single instance per
// Runtime. That is what makes the JIT match the interp, which binds each
// stdlib module once in the global environment every module can see.
inline std::string stdlib_preamble_for(
    const std::unordered_set<std::string_view>& names) {
  auto has = [&](std::string_view m) { return names.contains(m); };
  // Each module is pulled in when the program names it. Matching the parsed
  // token set rather than the raw source keeps a mention in a comment — or a
  // longer identifier that merely contains the name — from inlining a whole
  // module: "Terminal" in a comment used to pull in Term, and `side_effect`
  // the effects runtime, each about a second of JIT compile for nothing.
  // A `re'...'` / `re"..."` / `` re`...` `` regex literal desugars to
  // `Regex.compile(...)` in the parser, so it shows up as a plain `Regex`
  // token here and needs no extra marker.
  std::string preamble;
  for (const auto& m : lazy_ns_modules()) {
    if (has(m.name))
      preamble.append(_wrap_lazy_ns_module(m.source, m.name, m.builder));
  }
  // Bare-function modules: naming any one member pulls in its whole group,
  // since they share a source (one `assert_*` brings the family, as on the
  // interp side, where initialize_lazy_group binds all ten at once).
  for (const auto& g : lazy_fn_groups()) {
    if (std::any_of(g.members.begin(), g.members.end(), has))
      preamble.append(_wrap_lazy_fn_group(g));
  }
  return preamble;
}

// Path stamped on the synthesized preamble module, so it is distinguishable
// from user modules.
inline constexpr const char* kStdlibPreamblePath = "<stdlib>";

// JIT/AOT only: prepend the synthesized `<stdlib>` preamble module.
//
// The preamble registers the builders behind the helpers user code calls
// (assert_*, Time, …). The registrations are pure side effects with no scope
// bindings, so they run as their own module *ahead of every dependency* — a
// dependency's top-level `Canvas.rgba(...)` must find the builder already
// registered, and appending them to the entry module (which the loader
// schedules last) would run them too late for that. Every reference then
// resolves through the registry, so no module needs a copy of its own — the
// same reach the interpreter gets from its global environment.
inline void splice_stdlib_preamble(std::vector<LoadedModule>& modules) {
  if (modules.empty()) return;
  // Scan every module, not just the entry: an imported module's `Canvas`
  // (or `Time`, ...) must pull that namespace's preamble in too, even when
  // the entry module never names it.
  std::unordered_set<std::string_view> names;
  for (const auto& m : modules) {
    if (m.ast) collect_ast_tokens(*m.ast, names);
  }
  std::string preamble = stdlib_preamble_for(names);
  if (preamble.empty()) return;

  auto buf = std::make_shared<std::string>(std::move(preamble));
  std::vector<std::string> msgs;
  auto ast = parse_with_transforms(kStdlibPreamblePath, buf->data(),
                                   buf->size(), msgs);
  if (!ast) return;  // stdlib is trusted; a parse failure is a build bug
  LoadedModule pre;
  pre.abs_path = kStdlibPreamblePath;
  pre.source = std::move(buf);
  pre.ast = std::move(ast);
  modules.insert(modules.begin(), std::move(pre));
}

// Register stdlib modules that should not be parsed/evaluated up front.
// Each module is bound lazily so scripts that never touch it pay zero
// cost.
inline void register_stdlib_lazy_modules(Environment& env) {
  for (const auto& m : lazy_ns_modules()) env.initialize_lazy(m.name, m.source);
  // Bare-function modules (the 10 matchers, `replace`): every name in a group
  // shares one source via initialize_lazy_group. First `get` of any member
  // parses + evals the source once, binding the whole group in this env; the
  // others are picked up by the non-Nil guard in resolve_from_lazy. One
  // binding per env is what makes `assert_eq` read from two modules the same
  // function — the JIT reaches the same place through its builder registry
  // (see _wrap_lazy_fn_group).
  for (const auto& g : lazy_fn_groups()) {
    env.initialize_lazy_group({g.members.begin(), g.members.end()},
                              std::string(lazy_fn_group_source(g.name)));
  }
}

// Flag the root stdlib functions (namespace methods like `Math.abs` + bare
// globals like `type_of`) that declare a fixed positional signature as
// strict-arity, so invoke_user_function_with_args rejects the wrong
// positional count with the same count-based ArityError the JIT raises.
// Runs once, after setup and before any user code, so the synthesized native
// closures created during eval (enum/class constructors — body == nullptr,
// fixed params, otherwise indistinguishable) are never swept in. Variadic /
// zero-positional-param natives (min/max, range, iota, Tensor ctors) carry no
// cap and are left unflagged. Recurses into namespace objects (incl. nested,
// e.g. `Encoding.html`).
inline void _mark_strict_arity(Value& v) {
  if (v.type == Value::Function) {
    auto& f = v.get<FunctionValue>();
    if (f.body == nullptr && !f.is_builtin_method) {
      auto b = builtin_arity_bounds(*f.params);
      // Any fixed (non-variadic) signature is strict, including a genuine
      // 0-arity native like `GC.stat` — extra positionals are an ArityError,
      // matching the JIT (which rejects on its registered arity). The variadic
      // natives that read __ARGS__ (min/max, FS.join, Tensor ctors / eval) all
      // declare `*args`, so `!b.variadic` already excludes them.
      if (!b.variadic) f.strict_arity = true;
    }
  } else if (v.type == Value::Object) {
    auto& obj = v.get<ObjectValue>();
    if (obj.properties) {
      for (auto& entry : *obj.properties) _mark_strict_arity(entry.second.val);
    }
  }
}

inline void mark_strict_arity_builtins(Environment& env) {
  for (auto& [name, sym] : env.dictionary) _mark_strict_arity(sym.val);
}

// The program's arguments are not a parameter here: `Sys.argv` reads the
// process-wide `culebra::sys_argv()`, which the entry point fills in once
// (main.cc, the AOT bootstrap, or an embedder). This factory is also called
// lazily on worker threads — for the JIT's canonical env, for every isolate —
// where an argv parameter could only be a wrong answer.
inline std::shared_ptr<Environment> environment() {
  auto env = std::make_shared<Environment>();
  setup_core_globals(*env);
  setup_built_in_functions(*env);
  mark_strict_arity_builtins(*env);
  register_stdlib_lazy_modules(*env);
  return env;
}

// The single source of every name a fresh program can reference without
// declaring it: exactly what the global environment binds — eager
// dictionary entries (core globals, built-in functions, exception types,
// namespaces) plus the lazily-resolved stdlib modules. The static
// undefined-variable lint consults this so its notion of "builtin" cannot
// drift from what the runtime actually provides.
inline std::set<std::string, std::less<>> builtin_global_names() {
  auto env = environment();
  std::set<std::string, std::less<>> names;
  for (const auto& [name, sym] : env->dictionary) names.insert(name);
  for (const auto& [name, src] : env->lazy_pending) names.insert(name);
  // The CLI hoists these IO members to bare globals before running a
  // script (`install_cli_aliases` in main.cc), so a program sees them
  // without declaring them — the lint must treat them as builtins too.
  names.insert("inspect");
  names.insert("print");
  names.insert("println");
  return names;
}

// Install the builtin-name provider for the load-stage undefined-variable
// lint. lint.h lives below this layer (it can't build the set itself), so
// the entry point calls this once before loading any module — covering every
// backend, since all of them load through the shared module loader. The set
// is built once, lazily, and cached.
inline void install_undefined_var_lint() {
  lint::builtin_names_hook = [] {
    static const std::set<std::string, std::less<>> names =
        builtin_global_names();
    return &names;
  };
}

}  // namespace culebra

// Isolate / Channel stdlib + the sendable value-transfer layer. Included last
// because it depends on the full stdlib defined above (environment(),
// Interpreter). Provides the definition of make_isolate_namespace() declared
// near setup_built_in_functions.
#include "isolate.h"

namespace culebra {

// Per-worker state for Net's `listener.serve`: each worker thread rebuilds the
// handler onto its own heap here, and the on_conn trampoline calls into it.
// thread_local — one set per worker thread (same shape as the Http server's).
inline thread_local std::shared_ptr<Interpreter> g_net_w_interp;
inline thread_local std::shared_ptr<Environment> g_net_w_base;
inline thread_local Value g_net_w_handler;

inline Value _net_serve_impl(int64_t id, const Value& handler, int64_t workers,
                             int64_t line, int64_t col) {
  // Serialize once here, so a non-Sendable handler is an error at serve() —
  // not a surprise on the first connection. Each worker rebuilds it on its own
  // heap in setup().
  auto snode = std::make_shared<sendable::SendNode>();
  try {
    sendable::SerCtx sc;
    *snode = sendable::serialize(handler, sc);
  } catch (CulebraError& e) {
    throw CulebraError(
        "SendError",
        std::format("Net.serve: handler is not Sendable: {}", e.what()), line,
        col);
  }
  culebra::net::ServeHooks hooks;
  hooks.setup = [snode]() {
    g_net_w_interp = std::make_shared<Interpreter>();
    g_net_w_base = sendable::isolate_base_env();
    sendable::DeCtx dc;
    g_net_w_handler =
        sendable::deserialize(*snode, *g_net_w_interp, g_net_w_base, dc);
  };
  hooks.teardown = []() {
    g_net_w_handler = Value();
    g_net_w_base.reset();
    g_net_w_interp.reset();
  };
  hooks.on_conn = [](int64_t sid) {
    g_net_w_interp->call_closure(g_net_w_handler, g_net_w_base,
                                 {make_net_socket_handle(sid)});
  };
  std::string err;
  if (!culebra::net::serve(id, static_cast<int>(workers), hooks, &err))
    _net_throw("Net.serve", err, line, col);
  return Value();
}

#if defined(CULEBRA_HTTP_ENABLED)
// Per-worker dispatch state for a concurrent Http server (workers > 1): each
// worker thread rebuilds the route handlers onto its own heap here, then the
// trampolines call into it. thread_local — one set per worker thread.
inline thread_local std::shared_ptr<Interpreter> g_srv_w_interp;
inline thread_local std::shared_ptr<Environment> g_srv_w_base;
inline thread_local std::vector<Value> g_srv_w_handlers;

inline Value _http_server_do_serve(int64_t id, int64_t workers, bool async,
                                   const char* ctx, int64_t line, int64_t col) {
  std::string err;
  // Reject an unservable handle before touching the records, so a rejected
  // serve() leaves the routes registrable by a later one.
  if (!culebra::http::http_server_serve_ready(id, err))
    throw CulebraError("HttpError", std::format("{}: {}", ctx, err), line, col);
  // Handlers always run on a worker pool (off the accept loop) and so must be
  // Sendable: serialize each once here (that is the Sendable check), and each
  // worker thread rebuilds them onto its own heap. `async` only selects blocking
  // vs background serving below — not the handler model. Read the records in
  // place: a SendError must leave them recorded, as the JIT side does.
  {
    auto& recorded = g_srv_routes[id];
    auto snodes = std::make_shared<std::vector<sendable::SendNode>>();
    for (const auto& rec : recorded) {
      try {
        sendable::SerCtx sc;
        snodes->push_back(sendable::serialize(rec.handler, sc));
      } catch (CulebraError& e) {
        throw CulebraError(
            "SendError",
            std::format("{}: handler for {} {} is not Sendable: {}", ctx,
                        rec.method, rec.pattern, e.what()),
            line, col);
      }
    }
    // Past the Sendable check, move the records out of the thread_local map and
    // drop the entry: the RouteHandlers below capture the serialized
    // SendNodes, so the records are only needed during registration. Leaving
    // Values in a thread_local map past this point risks a double-free — the
    // map's destructor runs at thread exit, AFTER the GC heap has already torn
    // down (and freed those Values).
    std::vector<InterpRouteRecord> recs = std::move(recorded);
    g_srv_routes.erase(id);
    // One trampoline per route; each reads the calling worker thread's table.
    for (size_t i = 0; i < recs.size(); i++) {
      size_t ri = i;
      std::string rerr;
      if (recs[i].method == "WS") {
        // WebSocket: call the handler with (req, ws); it runs the connection's
        // loop. The ws handle is registered for the call and invalidated after
        // (its borrowed WebSocket dies when the handler returns).
        culebra::http::WsHandler wh =
            [ri](const httplib::Request& q, httplib::ws::WebSocket& ws) {
              int64_t wsid = culebra::http::ws_register_server(&ws);
              try {
                Value req = http_request_to_object(q);
                Value wsh = make_http_ws_handle(wsid, /*is_client=*/false);
                g_srv_w_interp->call_closure(g_srv_w_handlers[ri], g_srv_w_base,
                                             {req, wsh});
              } catch (...) {
                // Already upgraded — no HTTP response; just close the socket.
              }
              culebra::http::ws_unregister(wsid);
            };
        culebra::http::http_server_ws(id, recs[i].pattern, std::move(wh), rerr);
      } else {
        culebra::http::RouteHandler rh =
            [ri](const httplib::Request& q, httplib::Response& res) {
              try {
                Value req = http_request_to_object(q);
                Value ret = g_srv_w_interp->call_closure(g_srv_w_handlers[ri],
                                                         g_srv_w_base, {req});
                if (!http_try_stream_response(ret, res, g_srv_w_interp,
                                              g_srv_w_base))
                  http_apply_response(ret, res);
              } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(e.what(), "text/plain");
              }
            };
        culebra::http::http_server_route(id, recs[i].method, recs[i].pattern,
                                         std::move(rh), rerr);
      }
      if (!rerr.empty())
        throw CulebraError("HttpError", std::format("{}: {}", ctx, rerr), line,
                           col);
    }
    auto setup = [snodes]() {
      g_srv_w_interp = std::make_shared<Interpreter>();
      g_srv_w_base = sendable::isolate_base_env();
      g_srv_w_handlers.clear();
      g_srv_w_handlers.reserve(snodes->size());
      for (const auto& sn : *snodes) {
        sendable::DeCtx dc;
        g_srv_w_handlers.push_back(
            sendable::deserialize(sn, *g_srv_w_interp, g_srv_w_base, dc));
      }
    };
    auto teardown = []() {
      g_srv_w_handlers.clear();
      g_srv_w_base.reset();
      g_srv_w_interp.reset();
    };
    bool ok = async ? culebra::http::http_server_serve_async(
                          id, static_cast<int>(workers), setup, teardown, err)
                    : culebra::http::http_server_serve(
                          id, static_cast<int>(workers), setup, teardown, err);
    if (!ok)
      throw CulebraError("HttpError", std::format("{}: {}", ctx, err), line,
                         col);
    return Value();
  }
}
#endif  // CULEBRA_HTTP_ENABLED

}  // namespace culebra
