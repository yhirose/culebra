#pragma once

// Interpreter-side implementation of the Culebra standard library.
//
// Core built-ins bound on every environment: to_long, to_float,
// to_string, type_of (see docs/language.md §18). Everything else is
// grouped under a namespace ObjectValue: Math (abs/min/max/pow/sign/
// clamp/iota), IO (puts/print/input/read/write), Sys (argv/exit/env).
// The CLI (src/main.cc) additionally aliases IO.puts / IO.print as
// globals for scripting ergonomics; embedders get a clean environment
// by default.
//
// Independent header. Include from main.cc (or any embedder) after
// interpreter.h to wire stdlib into a fresh Environment via
// `culebra::environment(argv)` or `culebra::setup_built_in_functions(env)`.

#include <compress.h>
#include <csv.h>
#include <env.h>
#include <foreign.h>
#include <foreign_binding.h>
#include <hash.h>
#include <sqlite.h>
#include <toml.h>
#include <uuid.h>
#include <interpreter.h>
#include <proc.h>
#if defined(CULEBRA_HTTP_ENABLED)
#include <http.h>
#endif
#include <regexlib.h>
#include <term.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <iostream>
#include <limits>
#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>  // isatty (IO.*_is_terminal), chown (FS.chown)
#include <grp.h>     // getgrnam_r (FS.chown group-name resolution)
#include <pwd.h>     // getpwnam_r (FS.chown user-name resolution)
#include <sys/stat.h>  // ::stat for st_uid/st_gid (FS.stat owner fields)
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
                              auto v = x.get<long>();
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
    long line = env->get("__LINE__").to_long();
    long col = env->get("__COLUMN__").to_long();
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
    long acc = extras[0].get<long>();
    for (size_t i = 1; i < extras.size(); i++) {
      long x = extras[i].get<long>();
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
                            return Value(x > 0 ? 1L : (x < 0 ? -1L : 0L));
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
          return Value(static_cast<long>(fn(v.get<double>())));
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

  ns.initialize("pi",  Value(M_PI), false);
  ns.initialize("e",   Value(M_E), false);
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

  ns.initialize("puts",
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

  // Write to standard error — the twin of print / puts (no stderr writer
  // existed before). eputs quotes + newline like puts; eprint is raw.
  ns.initialize("eputs",
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

  // Terminal detection (POSIX isatty) per standard stream. Lets a script
  // branch on interactivity: prompt vs read a pipe (stdin), colorize vs emit
  // plain output (stdout/stderr). Mirrors Rust `io::stdin().is_terminal()` /
  // Node `process.stdin.isTTY`.
  auto is_terminal = [](int fd) {
    return Value(FunctionValue(
        {}, [fd](std::shared_ptr<Environment>) { return Value(isatty(fd) != 0); },
        "Bool"sv));
  };
  ns.initialize("stdin_is_terminal", is_terminal(STDIN_FILENO), false);
  ns.initialize("stdout_is_terminal", is_terminal(STDOUT_FILENO), false);
  ns.initialize("stderr_is_terminal", is_terminal(STDERR_FILENO), false);

  // File I/O lives on FS (FS.read / FS.write / FS.exists). IO is the
  // standard-stream + console namespace: puts / print / input.
  return Value(std::move(ns));
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
inline long _fs_mtime_secs(const std::filesystem::path& p) {
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
[[noreturn]] inline void _io_throw(const std::string& what, long line, long col,
                                   const std::error_code& ec = {}) {
  auto msg = ec ? std::format("{}: {}.", what, ec.message())
                : std::string(what);
  throw CulebraError("IOError", std::move(msg), line, col);
}

// --- FS ownership helpers (shared by interp + JIT/AOT so the three backends
// resolve names and report errors identically) ----------------------------

// Read st_uid / st_gid of `p` (following symlinks, like the other stat fields).
// Returns false if the path can't be stat'd.
inline bool _fs_owner(const std::filesystem::path& p, long& uid, long& gid) {
  struct ::stat st;
  if (::stat(p.c_str(), &st) != 0) return false;
  uid = static_cast<long>(st.st_uid);
  gid = static_cast<long>(st.st_gid);
  return true;
}

// Resolve a user name to a uid (thread-safe getpwnam_r), or -1 if unknown.
inline long _fs_uid_from_name(const std::string& name) {
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
inline long _fs_gid_from_name(const std::string& name) {
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
inline void _fs_do_chown(const std::filesystem::path& p, long uid, long gid,
                         long line, long col) {
  if (::chown(p.c_str(), static_cast<uid_t>(uid),
              static_cast<gid_t>(gid)) != 0) {
    _io_throw(std::format("FS.chown('{}')", p.string()), line, col,
              std::error_code(errno, std::generic_category()));
  }
}

inline Value make_fs_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  auto throw_io = [](const std::string& what, long line, long col,
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
        long line = env->get("__LINE__").to_long();
        long col = env->get("__COLUMN__").to_long();
        if (!env->has("__ARGS__")) return Value(std::string(""));
        const auto& args = *env->get("__ARGS__").to_array().values;
        std::filesystem::path out;
        for (const auto& v : args) {
          if (v.type != Value::String) throw_io("FS.join: non-String arg", line, col);
          out /= std::string(v.to_string());
        }
        return Value(out.string());
      })),
      false);

  auto string_to_string = [](auto fn) {
    return Value(FunctionValue(
        {{"path", false, "String"sv}},
        [fn](std::shared_ptr<Environment> env) {
          const auto& p = env->get("path").to_string();
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
        {{"path", false, "String"sv}},
        [fn](std::shared_ptr<Environment> env) {
          const auto& p = env->get("path").to_string();
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
      Value(FunctionValue({{"path", false, "String"sv}},
                          [throw_io](std::shared_ptr<Environment> env) {
                            long line = env->get("__LINE__").to_long();
                            long col = env->get("__COLUMN__").to_long();
                            const auto& p = env->get("path").to_string();
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
          {{"path", false, "String"sv}, {"content", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
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
          {{"path", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
            std::error_code ec;
            auto sz = std::filesystem::file_size(p, ec);
            if (ec) throw_io(std::format("FS.size('{}')", p), line, col, ec);
            return Value(static_cast<long>(sz));
          },
          "Long"sv)),
      false);

  ns.initialize(
      "list_dir",
      Value(FunctionValue(
          {{"path", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) -> Value {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
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
          {{"path", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
            std::error_code ec;
            std::filesystem::create_directories(p, ec);
            if (ec) throw_io(std::format("FS.mkdir('{}')", p), line, col, ec);
            return Value();
          })),
      false);

  ns.initialize(
      "remove",
      Value(FunctionValue(
          {{"path", false, "String"sv},
           {"recursive", false, ""sv, nullptr, kw_default_false()}},
          [throw_io](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
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
          {{"path", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) -> Value {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
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
            long mtime = _fs_mtime_secs(p);
            ObjectValue obj;
            obj.initialize("size", Value(static_cast<long>(sz)), false);
            obj.initialize("is_dir",
                           Value(std::filesystem::is_directory(fst)), false);
            obj.initialize("is_file",
                           Value(std::filesystem::is_regular_file(fst)),
                           false);
            obj.initialize("is_symlink", Value(is_link), false);
            obj.initialize("mtime", Value(mtime), false);
            obj.initialize(
                "mode",
                Value(static_cast<long>(fst.permissions() &
                                        std::filesystem::perms::mask)),
                false);
            long uid = -1, gid = -1;
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
          {{"path", false, "String"sv}, {"mode", false, "Long"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
            long mode = env->get("mode").to_long();
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
          {{"path", false, "String"sv},
           {"owner", false, ""sv, nullptr, kw_default_nil()},
           {"group", false, ""sv, nullptr, kw_default_nil()}},
          [](std::shared_ptr<Environment> env) -> Value {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
            auto resolve = [&](const Value& v, const char* param,
                               bool is_user) -> long {
              if (v.type == Value::Nil) return -1;
              if (v.type == Value::Long) return v.get<long>();
              if (v.is_stringlike()) {  // String or StringView (e.g. a slice)
                const auto name = v.to_string();
                long id = is_user ? _fs_uid_from_name(name)
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
            long uid = resolve(env->get("owner"), "owner", true);
            long gid = resolve(env->get("group"), "group", false);
            _fs_do_chown(std::filesystem::path(p), uid, gid, line, col);
            return Value();
          })),
      false);

  // `FS.rename(src, dst)` — atomic within a filesystem. IOError otherwise.
  ns.initialize(
      "rename",
      Value(FunctionValue(
          {{"src", false, "String"sv}, {"dst", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& s = env->get("src").to_string();
            const auto& d = env->get("dst").to_string();
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
          {{"src", false, "String"sv}, {"dst", false, "String"sv},
           {"recursive", false, ""sv, nullptr, kw_default_false()}},
          [throw_io](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& s = env->get("src").to_string();
            const auto& d = env->get("dst").to_string();
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
          {{"path", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
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
          {{"path", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
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
          {{"target", false, "String"sv}, {"link", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& t = env->get("target").to_string();
            const auto& l = env->get("link").to_string();
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
          {{"path", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
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
          {{"path", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) -> Value {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
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

[[noreturn]] inline void _file_throw(const std::string& what, long line,
                                     long col, std::string_view kind = "IOError") {
  throw CulebraError(std::string(kind), what, line, col);
}

// Open `path` in `mode` (r/w/a). Returns a fresh handle id. ValueError on
// bad mode, IOError if the stream can't be opened.
inline int64_t _file_open(const std::string& path, const std::string& mode,
                          long line, long col) {
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
inline _FileStream& _file_get(int64_t id, const char* op, long line, long col) {
  auto& tbl = _file_table();
  auto it = tbl.entries.find(id);
  if (it == tbl.entries.end()) {
    _file_throw(std::format("File.{}: operation on closed file", op), line, col);
  }
  return it->second;
}

inline std::string _file_read_all(int64_t id, long line, long col) {
  auto& s = _file_get(id, "read", line, col);
  if (!s.readable) _file_throw("File.read: file not opened for reading", line, col);
  std::string out((std::istreambuf_iterator<char>(s.fs)),
                  std::istreambuf_iterator<char>());
  return out;
}

inline std::string _file_read_n(int64_t id, long n, long line, long col) {
  auto& s = _file_get(id, "read", line, col);
  if (!s.readable) _file_throw("File.read: file not opened for reading", line, col);
  if (n < 0) n = 0;
  std::string buf(static_cast<size_t>(n), '\0');
  s.fs.read(buf.data(), n);
  buf.resize(static_cast<size_t>(s.fs.gcount()));
  return buf;
}

inline void _file_write(int64_t id, std::string_view data, long line, long col) {
  auto& s = _file_get(id, "write", line, col);
  if (!s.writable) _file_throw("File.write: file not opened for writing", line, col);
  s.fs.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (s.fs.bad()) _file_throw("File.write: write failed", line, col);
}

inline void _file_flush(int64_t id, long line, long col) {
  _file_get(id, "flush", line, col).fs.flush();
}

inline void _file_seek(int64_t id, long off, std::string_view whence,
                       long line, long col) {
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

inline long _file_tell(int64_t id, long line, long col) {
  auto& s = _file_get(id, "tell", line, col);
  return static_cast<long>(s.readable ? s.fs.tellg() : s.fs.tellp());
}

// Read one line (newline stripped, handles \n / \r\n / \r). Returns false
// at end of stream.
inline bool _file_getline(int64_t id, std::string& out, long line, long col) {
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
// handle on for-in exit (incl. break). `self` is captured only to keep the
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

[[noreturn]] inline void throw_value(const std::string& msg, long line, long col) {
  throw CulebraError("ValueError", msg, line, col);
}

inline long iso_weekday(const std::tm& tm) {
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
  if (utc) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
  return tm;
}

inline int64_t from_tm_nanos(std::tm& tm, int64_t sub_nanos, bool utc) {
  tm.tm_isdst = -1;
  auto t = utc ? timegm(&tm) : std::mktime(&tm);
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
  std::time_t t = timegm(&tm);
  if (has_tz) t -= offset_seconds;
  return combine_nanos(t, sub_ns);
}

inline std::string format_iso_nanos(int64_t nanos, bool utc) {
  auto [t, sub] = split_nanos(nanos);
  std::tm tm{};
  if (utc) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
  std::string tz_str = "Z";
  if (!utc) {
    auto offset = tm.tm_gmtoff;
    int sign = offset < 0 ? -1 : 1;
    long abs_off = std::abs(static_cast<long>(offset));
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
  if (utc) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
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
            return Value(static_cast<long>(ns_count));
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            auto r = _time_detail::parse_iso_nanos(s);
            if (!r) _time_detail::throw_value(
                std::format("_Time.from_iso_nanos: invalid ISO 8601 '{}'", s),
                line, col);
            return Value(static_cast<long>(*r));
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            std::tm tm{};
            if (!strptime(s.c_str(), fmt.c_str(), &tm)) {
              _time_detail::throw_value(
                  std::format("_Time.parse_nanos: '{}' does not match '{}'", s, fmt),
                  line, col);
            }
            tm.tm_isdst = -1;
            auto t = std::mktime(&tm);
            return Value(static_cast<long>(_time_detail::combine_nanos(t, 0)));
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
            p.initialize("year",      Value(static_cast<long>(tm.tm_year + 1900)), false);
            p.initialize("month",     Value(static_cast<long>(tm.tm_mon + 1)),     false);
            p.initialize("day",       Value(static_cast<long>(tm.tm_mday)),        false);
            p.initialize("hour",      Value(static_cast<long>(tm.tm_hour)),        false);
            p.initialize("minute",    Value(static_cast<long>(tm.tm_min)),         false);
            p.initialize("second",    Value(static_cast<long>(tm.tm_sec)),         false);
            p.initialize("nanosecond",Value(static_cast<long>(sub)),               false);
            p.initialize("weekday",   Value(_time_detail::iso_weekday(tm)),        false);
            p.initialize("dayofyear", Value(static_cast<long>(tm.tm_yday + 1)),    false);
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
            auto get = [&](const char* k, long fallback) -> long {
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
            return Value(static_cast<long>(_time_detail::from_tm_nanos(tm, sub_ns, utc)));
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
            long years_add  = env->get("years").to_long();
            long months_add = env->get("months").to_long();
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
            return Value(static_cast<long>(_time_detail::from_tm_nanos(tm, sub, utc)));
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
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
            return Value(static_cast<long>(_time_detail::from_tm_nanos(tm, 0, utc)));
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
            return Value(static_cast<long>(_term_detail::cols()));
          },
          "Long"sv)),
      false);
  ns.initialize("rows",
      Value(FunctionValue({},
          [](std::shared_ptr<Environment>) {
            return Value(static_cast<long>(_term_detail::rows()));
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
            return Value(static_cast<long>(_term_detail::color_level()));
          },
          "Long"sv)),
      false);

  // _Term.width(s: String) -> Long (display columns; wide/emoji = 2)
  ns.initialize("width",
      Value(FunctionValue({{"s", false, "String"sv}},
          [](std::shared_ptr<Environment> env) {
            return Value(static_cast<long>(
                _term_detail::width(env->get("s").to_string())));
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
            std::uniform_int_distribution<long> d(lo, hi - 1);
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

// If args[0] is a "f32"/"f64" tag, consume it; otherwise default F32.
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
    if (dt == Dtype::F32) _tensor_fill_1d(t->data_as<float>(), vs, line, col);
    else                  _tensor_fill_1d(t->data_as<double>(), vs, line, col);
    return t;
  }

  if (vs[0].type != Value::Array) throw_type_error_at(line, col);
  size_t cols = vs[0].to_array().values->size();
  size_t rows = vs.size();
  auto t = std::make_shared<TensorImpl>(
      TensorShape({static_cast<int64_t>(rows), static_cast<int64_t>(cols)}),
      dt);
  if (dt == Dtype::F32)
    _tensor_fill_2d(t->data_as<float>(), vs, cols, line, col);
  else
    _tensor_fill_2d(t->data_as<double>(), vs, cols, line, col);
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

  return Value(std::move(ns));
}

inline Value make_sys_namespace(const std::vector<std::string>& argv) {
  using namespace std::literals;
  ObjectValue ns;

  ArrayValue arr;
  arr.values->reserve(argv.size());
  for (const auto& s : argv) {
    arr.values->push_back(Value(std::string(s)));
  }
  ns.initialize("argv", Value(std::move(arr)), false);

  // Absolute path to the running culebra binary, for re-spawning a worker copy
  // of the interpreter (e.g. Proc.run([Sys.executable, "worker.cul"], ...)).
  ns.initialize("executable", Value(culebra::current_executable_path()), false);

  ns.initialize(
      "exit",
      Value(FunctionValue({{"code", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto code = env->get("code").to_long();
                            std::exit(static_cast<int>(code));
                            return Value();  // unreachable
                          })),
      false);

  ns.initialize(
      "env",
      Value(FunctionValue({{"name", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& name = env->get("name").to_string();
                            const char* v = std::getenv(name.c_str());
                            return Value(std::string(v ? v : ""));
                          },
                          "String"sv)),
      false);

  // Current working directory. IOError on failure (e.g. the cwd was
  // removed out from under the process).
  ns.initialize(
      "getcwd",
      Value(FunctionValue({},
                          [](std::shared_ptr<Environment> env) {
                            long line = env->get("__LINE__").to_long();
                            long col = env->get("__COLUMN__").to_long();
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
                            long line = env->get("__LINE__").to_long();
                            long col = env->get("__COLUMN__").to_long();
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& name = env->get("name").to_string();
            const auto& value = env->get("value").to_string();
            if (::setenv(name.c_str(), value.c_str(), 1) != 0) {
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
                            long live = static_cast<long>(gc.live_objects);
                            long bytes = static_cast<long>(gc.live_bytes);
                            ObjectValue stat;
                            stat.initialize("live_objects", Value(live), false);
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
inline Value make_shared_buffer_handle(long id, long count);

// `SharedBuffer.new(count, Cls)` allocates a flat, zero-initialized byte
// store holding `count` records laid out per the @packable class `Cls`,
// and returns a buffer handle. `buf[i]` yields a packed view whose
// `.field` reads/writes the backing bytes directly (zero copy). The
// native storage lives in a global registry; the handle just carries its
// integer id (a Value can't hold a raw shared_ptr). See
// [[project_packable_c3]].
inline Value make_shared_buffer_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize(
      "new",
      Value(FunctionValue(
          {{"count", false, ""sv}, {"type", false, ""sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            long line =
                env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            long col =
                env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
            long count = env->get("count").to_long();
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
            long id = culebra::make_shared_buffer(
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
            long line =
                env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            long col =
                env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
            const Value& pathv = env->get("path");
            if (!pathv.is_stringlike()) {
              throw culebra::CulebraError(
                  "TypeError", "SharedBuffer.file: path must be a String", line,
                  col);
            }
            std::string path(pathv.to_string_view());
            long count = env->get("count").to_long();
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
            long id = culebra::make_shared_buffer_file(
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
            long line =
                env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            long col =
                env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
            long count = env->get("count").to_long();
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
            long id = culebra::make_shared_buffer_shared(
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
            long line =
                env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            long col =
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
            long id = culebra::make_shared_buffer_from_share_env(
                *layout, cls, namev.to_string_view());
            auto core = culebra::lookup_shared_buffer(id);
            return make_shared_buffer_handle(
                id, core ? static_cast<long>(core->count) : 0);
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
    obj.initialize("code", Value(static_cast<long>(-1)), false);
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
    const std::vector<Value>& outer, const char* ctx, long line, long col) {
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
    const Value& share_v, std::string_view ctx, long line, long col,
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
    long id = sym.val.to_object().get("__sharedbuffer_id__").to_long();
    auto [fd, env_val] = culebra::prepare_share_buffer(id, k);
    env_out.emplace_back(culebra::share_env_key(k), std::move(env_val));
    fds_out.push_back(fd);
  }
}

// Validate and collect the cmd (non-empty Array<String>), cwd (nil/String),
// env (nil/Object of String) and stdin (String) kwargs. `ctx` tags errors.
inline ProcLaunchArgs proc_parse_launch(
    const std::shared_ptr<Environment>& env, const char* ctx, long line,
    long col) {
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
// to every holder of the handle (the user's variable and the bound `this`).
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
  h.initialize("_id", Value(static_cast<long>(id)), false);
  // A native handle (the fd lives in a process-local table) is not Sendable —
  // reject it at the serialize boundary the same way the JIT handle does.
  h.initialize("__nonsendable__", Value(true), false);

  auto hid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("this").to_object().get("_id").to_long();
  };
  auto loc = [](const std::shared_ptr<Environment>& env, long& line, long& col) {
    line = env->get("__LINE__").to_long();
    col = env->get("__COLUMN__").to_long();
  };

  // read(n: Long? = nil) — streaming. nil → rest of file, Long → ≤ n bytes.
  h.initialize(
      "read",
      Value(FunctionValue({{"n", false, ""sv, nullptr, kw_default_nil()}},
                          [hid, loc](std::shared_ptr<Environment> env) {
                            long line, col; loc(env, line, col);
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
                            long line, col; loc(env, line, col);
                            _file_write(hid(env), env->get("data").to_string(),
                                        line, col);
                            return Value();
                          })),
      false);

  h.initialize(
      "flush",
      Value(FunctionValue({}, [hid, loc](std::shared_ptr<Environment> env) {
        long line, col; loc(env, line, col);
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
            long line, col; loc(env, line, col);
            _file_seek(hid(env), env->get("offset").to_long(),
                       env->get("whence").to_string(), line, col);
            return Value();
          })),
      false);

  h.initialize(
      "tell",
      Value(FunctionValue({}, [hid, loc](std::shared_ptr<Environment> env) {
        long line, col; loc(env, line, col);
        return Value(_file_tell(hid(env), line, col));
      }, "Long"sv)),
      false);

  // lines() — line iterator (newline stripped). dispose closes the handle
  // so a broken for-in still releases the fd.
  h.initialize(
      "lines",
      Value(FunctionValue({}, [hid, loc](std::shared_ptr<Environment> env) {
        long line, col; loc(env, line, col);
        int64_t id = hid(env);
        // Capture the handle Value so the iterator keeps it alive — without
        // this the anonymous `File.open(p).lines()` handle would be GC'd
        // (drop → close) before the loop runs.
        Value self = env->get("this");
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
        long line, col; loc(env, line, col);
        int64_t id = hid(env);
        long n = env->get("n").to_long();
        Value self = env->get("this");
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
          {{"path", false, "String"sv},
           {"mode", false, ""sv, nullptr,
            std::make_shared<Value>(std::string("r"))}},
          [](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            int64_t id = _file_open(env->get("path").to_string(),
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
          {{"path", false, "String"sv},
           {"mode", false, ""sv, nullptr,
            std::make_shared<Value>(std::string("r"))},
           {"fn", false, "Function"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            int64_t id = _file_open(env->get("path").to_string(),
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

// ===========================================================================
// SQLite — embedded SQL database over the value-neutral cursor core (sqlite.h).
// `SQLite.open(path)` returns a Database handle; db.execute/query/transaction
// run high-level SQL; db.prepare returns a reusable Statement handle. Handles
// are __nonsendable__ (the sqlite3*/sqlite3_stmt* live in a thread-local table)
// and close deterministically on scope exit via the `drop` backstop.
// ===========================================================================

[[noreturn]] inline void _sqlite_throw(const std::string& msg, long line,
                                       long col) {
  throw CulebraError("SQLiteError", std::format("SQLite: {}", msg), line, col);
}

// culebra value -> neutral BindVal. Long/Bool -> Integer, Float -> Float,
// String -> Text, nil -> Null; anything else is a TypeError.
inline culebra::sqlite::BindVal _sqlite_to_bind(const Value& v, long line,
                                                long col) {
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
      return Value(static_cast<long>(c.i));
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
inline void _sqlite_bind_params(int64_t stmt_id, const Value& params, long line,
                                long col) {
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
inline Value _sqlite_collect_rows(int64_t stmt_id, long line, long col) {
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
inline void _sqlite_drain(int64_t stmt_id, long line, long col) {
  std::string err;
  for (;;) {
    int rc = culebra::sqlite::step(stmt_id, &err);
    if (rc < 0) _sqlite_throw(err, line, col);
    if (rc == 0) break;
  }
}

// execute(db, sql, params) -> rows affected. Prepares a transient statement.
inline Value _sqlite_execute(int64_t db_id, const std::string& sql,
                             const Value& params, long line, long col) {
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
  return Value(static_cast<long>(culebra::sqlite::changes(db_id)));
}

// query(db, sql, params) -> [Object]. Prepares a transient statement.
inline Value _sqlite_query(int64_t db_id, const std::string& sql,
                           const Value& params, long line, long col) {
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
  h.initialize("_id", Value(static_cast<long>(stmt_id)), false);
  h.initialize("__nonsendable__", Value(true), false);
  // Keep the owning Database alive while this Statement exists.
  h.initialize("__parent__", db_handle, false);

  auto sid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("this").to_object().get("_id").to_long();
  };
  auto dbid = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("this").to_object().get("__parent__").to_object().get("_id").to_long();
  };
  auto loc = [](const std::shared_ptr<Environment>& env, long& line, long& col) {
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
            long line, col; loc(env, line, col);
            int64_t st = sid(env);
            culebra::sqlite::reset(st);
            _sqlite_bind_params(st, env->get("params"), line, col);
            _sqlite_drain(st, line, col);
            return Value(static_cast<long>(culebra::sqlite::changes(dbid(env))));
          },
          "Long"sv)),
      false);

  // query(params=nil) -> [Object].
  h.initialize(
      "query",
      Value(FunctionValue(
          {params_param},
          [sid, loc](std::shared_ptr<Environment> env) -> Value {
            long line, col; loc(env, line, col);
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
  h.initialize("_id", Value(static_cast<long>(db_id)), false);
  h.initialize("__nonsendable__", Value(true), false);

  auto did = [](const std::shared_ptr<Environment>& env) -> int64_t {
    return env->get("this").to_object().get("_id").to_long();
  };
  auto loc = [](const std::shared_ptr<Environment>& env, long& line, long& col) {
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
            long line, col; loc(env, line, col);
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
            long line, col; loc(env, line, col);
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
            long line, col; loc(env, line, col);
            std::string err;
            int64_t st =
                culebra::sqlite::prepare(did(env), env->get("sql").to_string(), &err);
            if (st < 0) _sqlite_throw(err, line, col);
            return make_sqlite_stmt_handle(st, env->get("this"));
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
            long line, col; loc(env, line, col);
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
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

// Build the Proc.spawn live handle: data fields `_pid/_out/_err/_done/_result`
// plus `wait`/`poll`/`kill`/`drop` methods. The result is cached on first
// wait/poll so the methods are idempotent; `drop` (called on GC) best-effort
// reaps a child that was never waited on.
inline Value make_proc_handle(long pid, int out_fd, int err_fd) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("_pid", Value(pid), true);
  h.initialize("_out", Value(static_cast<long>(out_fd)), true);
  h.initialize("_err", Value(static_cast<long>(err_fd)), true);
  h.initialize("_done", Value(false), true);
  h.initialize("_result", Value(), true);
  // A native handle (pid + pipe fds are process-local) is not Sendable — reject
  // it at the serialize boundary the same way the JIT handle does.
  h.initialize("__nonsendable__", Value(true), false);

  h.initialize("wait",
      Value(FunctionValue({}, [](std::shared_ptr<Environment> env) -> Value {
        const Value& self = env->get("this");
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
        const Value& self = env->get("this");
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
      std::make_shared<Value>(Value(static_cast<long>(15)));
  h.initialize("kill",
      Value(FunctionValue({{"sig", false, "Long"sv, nullptr, kill_sig_default}},
          [](std::shared_ptr<Environment> env) -> Value {
            const auto& o = env->get("this").to_object();
            if (!o.get("_done").to_bool()) {
              culebra::proc::kill_pid(
                  o.get("_pid").to_long(),
                  static_cast<int>(env->get("sig").to_long()));
            }
            return Value();
          }, "Nil"sv)), false);

  h.initialize("drop",
      Value(FunctionValue({}, [](std::shared_ptr<Environment> env) -> Value {
        const Value& self = env->get("this");
        const auto& o = self.to_object();
        if (o.get("_done").to_bool()) return Value();
        int out_fd = static_cast<int>(o.get("_out").to_long());
        int err_fd = static_cast<int>(o.get("_err").to_long());
        long pid = o.get("_pid").to_long();
        culebra::proc::kill_pid(pid, SIGKILL);
        culebra::proc::wait_handle(pid, out_fd, err_fd);  // drains + reaps
        _proc_handle_set(self, "_done", Value(true));
        return Value();
      }, "Nil"sv)), false);

  return Value(std::move(h));
}

#if defined(CULEBRA_HTTP_ENABLED)
// Defined later in this header; forward-declared for the Http helpers' `json:`
// kwarg and `r.json()` method.
inline std::string json_stringify(const Value& v, int indent, bool sort_keys,
                                  int depth);
inline Value json_parse(std::string_view s, std::string_view number_mode = "auto",
                        bool jsonc = false);

// Convert the optional `headers` kwarg (nil or an Object of String values)
// into the header list the http core wants. `ctx` tags type errors.
inline culebra::http::HeaderList http_parse_headers(const Value& hv,
                                                    const char* ctx, long line,
                                                    long col) {
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
                                                   const char* ctx, long line,
                                                   long col) {
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
                                                 const char* ctx, long line,
                                                 long col) {
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
        return json_parse(env->get("this").to_object().get("body")
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
                             long line, long col) {
  req.headers = http_parse_headers(env->get("headers"), ctx, line, col);
  req.params = http_parse_params(env->get("params"), ctx, line, col);
  // nil means "unset" → use the default (matching every other optional kwarg
  // here and the JIT, which can't tell an explicit `timeout: nil` from a
  // positionally-absent slot). A present non-nil still goes through the strict
  // to_long()/to_bool() coercion.
  const auto& tv = env->get("timeout");
  long timeout = tv.type == Value::Nil ? 0 : tv.to_long();
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
                            const char* ctx, long line, long col) {
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
                           const char* ctx, long line, long col) {
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
                                  HttpIntoState& st, const char* ctx, long line,
                                  long col) {
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
                                  HttpIntoState& st, const char* ctx, long line,
                                  long col) {
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
                            const char* ctx, long line, long col) {
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
    req.body = json_stringify(jsonv, 0, false, 0);
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
  h.initialize("_id", Value(static_cast<long>(id)), false);
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
    return env->get("this").to_object().get("_id").to_long();
  };

  // get / delete / head — no body.
  auto bodyless = [&](const char* method, const char* ctx) {
    return Value(FunctionValue(
        {path_param, headers_param, params_param, into_param},
        [method, ctx, cid](std::shared_ptr<Environment> env) -> Value {
          long line = env->get("__LINE__").to_long();
          long col = env->get("__COLUMN__").to_long();
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
          long line = env->get("__LINE__").to_long();
          long col = env->get("__COLUMN__").to_long();
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
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
          long line = env->get("__LINE__").to_long();
          long col = env->get("__COLUMN__").to_long();
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
          long line = env->get("__LINE__").to_long();
          long col = env->get("__COLUMN__").to_long();
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            culebra::http::HttpRequest req;
            req.method = "GET";
            req.url = env->get("url").to_string();
            const Value cb = env->get("on_event");
            req.headers =
                http_parse_headers(env->get("headers"), "Http.sse", line, col);
            const auto& tv = env->get("timeout");
            long timeout = tv.type == Value::Nil ? 0 : tv.to_long();
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            auto headers =
                http_parse_headers(env->get("headers"), "Http.client", line, col);
            const auto& tv = env->get("timeout");
            long timeout = tv.type == Value::Nil ? 0 : tv.to_long();
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

  return Value(std::move(ns));
}
#endif  // CULEBRA_HTTP_ENABLED

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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();

            auto la = proc_parse_launch(env, "Proc.run", line, col);
            bool check = env->get("check").to_bool();
            long timeout = env->get("timeout").to_long();
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            auto commands = proc_parse_command_list(
                *env->get("commands").to_array().values, "Proc.all", line, col);
            long lim = env->get("limit").to_long();
            if (lim < 0) lim = 0;
            long timeout = env->get("timeout").to_long();
            if (timeout < 0) timeout = 0;
            bool fail_fast = env->get("fail_fast").to_bool();
            long retries = env->get("retries").to_long();
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
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
inline std::string json_stringify(const Value& v, int indent = 0,
                                   bool sort_keys = false,
                                   int depth = 0) {
  auto sep = [&](int level) -> std::string {
    if (indent <= 0) return "";
    return std::string("\n") + std::string(indent * level, ' ');
  };
  const char* colon = indent > 0 ? ": " : ":";
  switch (v.type) {
    case Value::Nil:    return "null";
    case Value::Bool:   return v.get<bool>() ? "true" : "false";
    case Value::Long:   return std::to_string(v.get<long>());
    case Value::Float: {
      double d = v.get<double>();
      if (!std::isfinite(d)) {
        throw CulebraError("ValueError",
                           "JSON.stringify: non-finite Float");
      }
      return format_float_shortest(d);
    }
    case Value::String: return culebra::json_escape(v.get<std::string>());
    case Value::Array:
    case Value::Tuple:
    case Value::Set: {
      // Array, Tuple, and Set all render as JSON arrays. Set uses the
      // members vector (insertion order).
      const std::vector<Value>* xs = nullptr;
      if (v.type == Value::Array) xs = v.to_array().values.get();
      else if (v.type == Value::Tuple) xs = v.template get<TupleValue>().elements.get();
      else                       xs = v.template get<SetValue>().members.get();
      if (xs->empty()) return "[]";
      std::string s = "[";
      for (size_t i = 0; i < xs->size(); i++) {
        if (i) s += ",";
        s += sep(depth + 1);
        s += json_stringify((*xs)[i], indent, sort_keys, depth + 1);
      }
      s += sep(depth);
      return s + "]";
    }
    case Value::Object: {
      const auto& obj = v.to_object();
      // Reject Objects carrying non-String keys (Long/Tuple/etc.) so
      // stringify ↔ parse stays a clean round trip.
      if (obj.non_string_props && !obj.non_string_props->empty()) {
        throw CulebraError("TypeError",
            "JSON.stringify: Object has non-String keys");
      }
      if (obj.properties->empty()) return "{}";
      // Collect entries in iteration order, then sort if requested.
      std::vector<std::pair<std::string_view, const Value*>> entries;
      entries.reserve(obj.properties->size());
      for (const auto& [k, sym] : *obj.properties) {
        entries.emplace_back(k, &sym.val);
      }
      if (sort_keys) {
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
      }
      std::string s = "{";
      bool first = true;
      for (const auto& [k, val_ptr] : entries) {
        if (!first) s += ",";
        first = false;
        s += sep(depth + 1);
        s += culebra::json_escape(k) + colon +
             json_stringify(*val_ptr, indent, sort_keys, depth + 1);
      }
      s += sep(depth);
      return s + "}";
    }
    default:
      throw CulebraError("TypeError", std::format(
          "JSON.stringify: cannot serialize {}", v.type_name()));
  }
}

// JSON Lines: each element on its own compact line, terminated by `\n`.
// `v` must be an Array/Tuple/Set; mixing with `indent > 0` is illegal.
inline std::string json_stringify_lines(const Value& v, bool sort_keys = false) {
  const std::vector<Value>* xs = nullptr;
  if (v.type == Value::Array) xs = v.to_array().values.get();
  else if (v.type == Value::Tuple) xs = v.template get<TupleValue>().elements.get();
  else if (v.type == Value::Set) xs = v.template get<SetValue>().members.get();
  else {
    throw CulebraError("TypeError", std::format(
        "JSON.stringify(lines: true): expects Array/Tuple/Set, got {}",
        v.type_name()));
  }
  std::string s;
  for (const auto& elem : *xs) {
    s += json_stringify(elem, /*indent=*/0, sort_keys);
    s += '\n';
  }
  return s;
}

// Minimal recursive-descent JSON parser. Tracks 1-based line/col so
// `JSON.parse(bad).catch e` exposes `e.line` / `e.col` pointing at the
// offending character.
struct _JsonParser {
  const char* p;
  const char* end;
  long line = 1;
  long col = 1;
  // 'auto' (default): integer-shaped → Long, otherwise Float.
  // 'float'         : every number → Float.
  std::string_view number_mode = "auto";
  // JSONC tolerance: `//` and `/* */` comments + trailing commas. Off by
  // default so strict JSON stays strict (an unexpected `//` is an error).
  bool jsonc = false;

  void advance() {
    if (*p == '\n') { line++; col = 1; }
    else            { col++; }
    ++p;
  }
  void skip_ws() {
    while (p < end) {
      char c = *p;
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { advance(); continue; }
      if (jsonc && c == '/' && p + 1 < end) {
        if (p[1] == '/') {                 // line comment → to end of line
          advance(); advance();
          while (p < end && *p != '\n') advance();
          continue;
        }
        if (p[1] == '*') {                 // block comment → to closing */
          advance(); advance();
          while (p < end && !(*p == '*' && p + 1 < end && p[1] == '/')) advance();
          if (p < end) { advance(); advance(); }
          continue;
        }
      }
      break;
    }
  }
  [[noreturn]] void fail(const char* msg) {
    // Format with " at L:C." inline so eval()'s catch doesn't replace
    // our JSON-internal location with the caller AST's location.
    throw CulebraError("ValueError",
                       std::format("JSON.parse: {}", msg), line, col);
  }
  Value parse_value() {
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
  Value parse_object() {
    advance(); skip_ws();
    ObjectValue obj;
    if (p < end && *p == '}') { advance(); return Value(std::move(obj)); }
    while (p < end) {
      skip_ws();
      // JSONC: a `,` may be followed by `}` (trailing comma).
      if (jsonc && p < end && *p == '}') { advance(); return Value(std::move(obj)); }
      auto key = parse_string_raw();
      skip_ws();
      if (p >= end || *p != ':') fail("expected ':'");
      advance();
      auto val = parse_value();
      // OrderedSymbolMap owns keys (post-K) so a transient std::string
      // is safe — its bytes are copied into the map's stable index.
      obj.initialize(std::string_view(key), val, false);
      skip_ws();
      if (p < end && *p == ',') { advance(); continue; }
      if (p < end && *p == '}') { advance(); return Value(std::move(obj)); }
      fail("expected ',' or '}'");
    }
    fail("unterminated object");
  }
  Value parse_array() {
    advance(); skip_ws();
    ArrayValue arr;
    if (p < end && *p == ']') { advance(); return Value(std::move(arr)); }
    while (p < end) {
      skip_ws();
      // JSONC: a `,` may be followed by `]` (trailing comma).
      if (jsonc && p < end && *p == ']') { advance(); return Value(std::move(arr)); }
      arr.values->push_back(parse_value());
      skip_ws();
      if (p < end && *p == ',') { advance(); continue; }
      if (p < end && *p == ']') { advance(); return Value(std::move(arr)); }
      fail("expected ',' or ']'");
    }
    fail("unterminated array");
  }
  Value parse_string() { return Value(parse_string_raw()); }
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
  Value parse_bool() {
    if (end - p >= 4 && std::string_view(p, 4) == "true") {
      for (int i = 0; i < 4; i++) advance();
      return Value(true);
    }
    if (end - p >= 5 && std::string_view(p, 5) == "false") {
      for (int i = 0; i < 5; i++) advance();
      return Value(false);
    }
    fail("bad bool");
  }
  Value parse_null() {
    if (end - p >= 4 && std::string_view(p, 4) == "null") {
      for (int i = 0; i < 4; i++) advance();
      return Value();
    }
    fail("bad null");
  }
  Value parse_number() {
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
      return Value(std::stod(buf));
    }
    if (is_float) return Value(std::stod(buf));
    return Value(static_cast<long>(std::stoll(buf)));
  }
};

inline Value json_parse(std::string_view s, std::string_view number_mode,
                        bool jsonc) {
  _JsonParser jp{s.data(), s.data() + s.size()};
  jp.number_mode = number_mode;
  jp.jsonc = jsonc;
  auto v = jp.parse_value();
  jp.skip_ws();
  if (jp.p != jp.end) {
    throw CulebraError("ValueError",
                       "JSON.parse: trailing characters", jp.line, jp.col);
  }
  return v;
}

// JSON Lines: split `s` on `\n`, parse each non-empty line as its own
// JSON value, return an Array. The outer-error line number aligns with
// the JSONL line that failed (the inner parser sees 1-based line within
// the slice; we shift back to the overall line on error).
inline Value json_parse_lines(std::string_view s,
                               std::string_view number_mode = "auto",
                               bool jsonc = false) {
  ArrayValue arr;
  long lineno = 1;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find('\n', i);
    auto slice =
        s.substr(i, j == std::string_view::npos ? s.size() - i : j - i);
    if (!slice.empty()) {
      _JsonParser jp{slice.data(), slice.data() + slice.size()};
      jp.line = lineno;
      jp.number_mode = number_mode;
      jp.jsonc = jsonc;
      arr.values->push_back(jp.parse_value());
      jp.skip_ws();
      if (jp.p != jp.end) {
        throw CulebraError("ValueError",
                           "JSON.parse(lines: true): trailing characters",
                           jp.line, jp.col);
      }
    }
    if (j == std::string_view::npos) break;
    i = j + 1;
    lineno++;
  }
  return Value(std::move(arr));
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
            int indent = static_cast<int>(env->get("indent").get<long>());
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
// is nil. Flags are inline ((?i)/(?m)/(?s)). See docs/regex_stdlib_api.md.
//===--------------------------------------------------------------------===//

// A capture group -> { value, start, end }, or nil if it did not participate.
// `offset` shifts the byte spans to absolute positions (find_from searches a
// suffix and reports relative offsets); the new engine's match views are
// immutable, so the shift is applied here at build time rather than by mutating.
inline Value regex_group_value(const reg::Match& c, size_t offset = 0) {
  if (!c.matched()) return Value();
  ObjectValue g;
  g.initialize("value", Value(std::string(c.str())), false);
  g.initialize("start", Value(static_cast<long>(c.begin() + offset)), false);
  g.initialize("end", Value(static_cast<long>(c.end() + offset)), false);
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
  mo.initialize("start", Value(static_cast<long>(m.begin() + offset)), false);
  mo.initialize("end", Value(static_cast<long>(m.end() + offset)), false);
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
    const std::string& pattern, long line, long col) {
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
                            long line = env->get("__LINE__").to_long();
                            long col = env->get("__COLUMN__").to_long();
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
                            long line = env->get("__LINE__").to_long();
                            long col = env->get("__COLUMN__").to_long();
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
  return Value(std::move(ns));
}

// `UUID`: generate canonical UUIDs. v4 = random, v7 = time-ordered (Unix-ms
// prefix). Entropy from the shared PRNG (csv-style value-neutral core in
// uuid.h shared with the JIT slow-path adapters).
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
    case CT::Long:   return Value(static_cast<long>(cc.long_val));
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
                               bool header, const Value& typesv, long line,
                               long col) {
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
                            long line = env->get("__LINE__").to_long();
                            long col = env->get("__COLUMN__").to_long();
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
                            long line = env->get("__LINE__").to_long();
                            long col = env->get("__COLUMN__").to_long();
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
    case K::Int:    return Value(static_cast<long>(n.i));
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
inline culebra::toml::Node value_to_toml_node(const Value& v, long line,
                                              long col) {
  using N = culebra::toml::Node;
  switch (v.type) {
    case Value::Bool:   return N::boolean(v.get<bool>());
    case Value::Long:   return N::integer(v.get<long>());
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
                            long line = env->get("__LINE__").to_long();
                            long col = env->get("__COLUMN__").to_long();
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
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
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
              ::setenv(k.c_str(), v.c_str(), overwrite ? 1 : 0);
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
                            long line = env->get("__LINE__").to_long();
                            long col = env->get("__COLUMN__").to_long();
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
                            long line = env->get("__LINE__").to_long();
                            long col = env->get("__COLUMN__").to_long();
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
  // lazy `Regex.find_iter` generator: the leftmost match at/after byte `pos`
  // (absolute offsets) plus the grapheme-correct resume position — one
  // grapheme past an empty match, so the generator always advances.
  ns.initialize(
      "find_from",
      Value(FunctionValue(
          {{"pattern", false, "StringLike"sv},
           {"s", false, "StringLike"sv},
           {"pos", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            auto re = regex_from_env(env);
            std::string s{env->get("s").to_string()};
            long pos = env->get("pos").to_long();
            ObjectValue out;
            auto miss = [&](long nxt) {
              out.initialize("m", Value(), false);
              out.initialize("nxt", Value(nxt), false);
              return Value(std::move(out));
            };
            if (pos < 0 || static_cast<size_t>(pos) > s.size())
              return miss(static_cast<long>(s.size()) + 1);
            std::string_view suffix = std::string_view(s).substr(pos);
            try {
              auto m = re->search(suffix);
              if (!m.matched()) return miss(static_cast<long>(s.size()) + 1);
              long next;
              if (m.end() > m.begin()) {
                next = pos + static_cast<long>(m.end());
              } else {
                // Empty match: resume one grapheme past it so the generator
                // always advances. byte_begin lists the grapheme boundaries
                // (sorted, sentinel == suffix.size()); the first one strictly
                // after m.end() is the next grapheme start.
                auto seg = reg::detail::segment(suffix);
                auto it = std::upper_bound(seg.byte_begin.begin(),
                                           seg.byte_begin.end(), m.end());
                size_t nb =
                    it != seg.byte_begin.end() ? *it : suffix.size() + 1;
                next = pos + static_cast<long>(nb);
              }
              // The match offsets are relative to `suffix`; shift to absolute by
              // pos (the engine's result is immutable, so shift at build time).
              out.initialize("m",
                             regex_match_value(m, re->named_groups(),
                                               static_cast<size_t>(pos)),
                             false);
              out.initialize("nxt", Value(next), false);
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
                              Value(static_cast<long>(m.begin())));
                          av.values->push_back(
                              Value(static_cast<long>(m.end())));
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
                        return Value(static_cast<long>(re->find_all(s).size()));
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

inline void setup_built_in_functions(
    Environment& env, const std::vector<std::string>& argv = {}) {
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
                              return Value(static_cast<long>(v.get<double>()));
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
                              return Value(static_cast<double>(v.get<long>()));
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
                  return Value(static_cast<long>(ValueHash{}(v)));
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

  env.initialize("Math", make_math_namespace(), false);
  env.initialize("IO", make_io_namespace(), false);
  env.initialize("FS", make_fs_namespace(), false);
  env.initialize("File", make_file_namespace(), false);
  // Wrapped C++ classes (wrap.h declarations, e.g. foreign_binding.h's
  // `__Foreign.Counter`): one namespace object per declared ns, holding
  // one class object (ctor + statics) per class. Registered at
  // static-init time, so the registry is complete before setup runs.
  {
    std::map<std::string, ObjectValue> wrapped_ns;
    for (const auto& wc : wrapped_classes()) {
      wrapped_ns[wc.ns].initialize(wc.name, wc.build_class_object(), false);
    }
    for (auto& [ns, obj] : wrapped_ns) {
      env.initialize(ns, Value(std::move(obj)), false);
    }
  }
  env.initialize("_Time", make_time_primitives_namespace(), false);
  env.initialize("_Term", make_term_primitives_namespace(), false);
  env.initialize("Random", make_random_namespace(), false);
  env.initialize("Sys", make_sys_namespace(argv), false);
  env.initialize("GC", make_gc_namespace(), false);
  env.initialize("Tensor", make_tensor_namespace(), false);
  env.initialize("JSON", make_json_namespace(), false);
  env.initialize("Encoding", make_encoding_namespace(), false);
  env.initialize("Compress", make_compress_namespace(), false);
  env.initialize("Hash", make_hash_namespace(), false);
  env.initialize("CSV", make_csv_namespace(), false);
  env.initialize("SQLite", make_sqlite_namespace(), false);
  env.initialize("TOML", make_toml_namespace(), false);
  env.initialize("Env", make_env_namespace(), false);
  env.initialize("UUID", make_uuid_namespace(), false);
  env.initialize("_Regex", make_regex_primitives_namespace(), false);
  env.initialize("Proc", make_proc_namespace(), false);
#if defined(CULEBRA_HTTP_ENABLED)
  env.initialize("Http", make_http_namespace(), false);
#endif
  env.initialize("Isolate", make_isolate_namespace(), false);
  env.initialize("Channel", make_channel_namespace(), false);
  env.initialize("Signal", make_signal_namespace(), false);
  env.initialize("Parallel", make_parallel_namespace(), false);
  env.initialize("SharedBuffer", make_shared_buffer_namespace(), false);
  env.initialize("Shared", make_shared_namespace(), false);
}

// Embedded culebra source for stdlib modules that are easier to express
// in culebra than in C++. Both Time and Args are registered lazily by
// `environment()` — see [[project-startup-overhead]]. The JIT path
// still pre-concats them via `STDLIB_PREAMBLE_SOURCE` until it adopts
// the env's lazy bindings. Each module is single-line so user-code
// error positions are only off-by-one.
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
// colored when stderr is a tty. Single-line so JIT/AOT preamble prepending
// shifts user-code error lines by only one, matching the other modules.

// Transitional concatenation used by the JIT path until it adopts the
// env's lazy bindings (Phase 3 of [[project-startup-overhead]]). The
// interp path is already preamble-free.
inline const char* _stdlib_preamble_concat() {
  static const std::string s =
      std::string(TIME_MODULE_SOURCE) + ARGS_MODULE_SOURCE +
      MATCHERS_MODULE_SOURCE + REGEX_MODULE_SOURCE +
      STRING_REPLACE_MODULE_SOURCE;
  return s.c_str();
}
inline const char* STDLIB_PREAMBLE_SOURCE = _stdlib_preamble_concat();

// Build the selective stdlib preamble for a script: only the modules the
// source appears to reference (substring match). The JIT/AOT backends
// need these helpers inlined into the entry module because they don't
// honour the env's lazy bindings (see Phase 3 of
// [[project-startup-overhead]]); the interpreter binds them lazily and
// never calls this. A substring hit is a safe over-approximation: false
// positives (e.g. `let myTime = 1`) just include an unneeded module; only
// true negatives skip a module, preserving correctness.
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

inline std::string stdlib_preamble_for(std::string_view user_src) {
  auto has = [&](std::string_view m) {
    return user_src.find(m) != std::string_view::npos;
  };
  // Each module is pulled in when its marker appears anywhere in the
  // source; `assert_` pulls the whole matcher family (no other stdlib
  // symbol starts with it). A `re'...'` / `re"..."` / `` re`...` `` regex
  // literal desugars to `Regex.compile(...)` (see parser.h) but leaves no
  // `Regex` substring in the source, so its prefixes are extra markers for
  // the Regex module. Over-approximation is safe — a false positive (e.g.
  // "there's" matching `re'`) just inlines an unused module.
  //
  // The five namespace modules (Time/Term/Args/Regex/Log) are wrapped as lazy
  // builder registrations (see _wrap_lazy_ns_module); the matcher family and
  // `replace` (a bare fn, not a namespace) stay plain spliced bindings.
  std::string preamble;
  if (has("Time"))
    preamble.append(_wrap_lazy_ns_module(TIME_MODULE_SOURCE, "Time",
                                         "_time_module"));
  if (has("Term"))
    preamble.append(_wrap_lazy_ns_module(TERM_MODULE_SOURCE, "Term",
                                         "_term_module"));
  if (has("Args"))
    preamble.append(_wrap_lazy_ns_module(ARGS_MODULE_SOURCE, "Args",
                                         "_args_module"));
  if (has("assert_")) preamble.append(MATCHERS_MODULE_SOURCE);
  if (has("Regex") || has("re'") || has("re\"") || has("re`"))
    preamble.append(_wrap_lazy_ns_module(REGEX_MODULE_SOURCE, "Regex",
                                         "_regex_module"));
  if (has("replace")) preamble.append(STRING_REPLACE_MODULE_SOURCE);
  if (has("Log"))
    preamble.append(_wrap_lazy_ns_module(LOG_MODULE_SOURCE, "Log",
                                         "_log_module"));
  return preamble;
}

// Collect the top-level statements from a parsed program AST. After the
// AstOptimizer runs, a multi-statement program's root carries the
// STATEMENTS tag (its children are the statements), while a single-
// statement program collapses so that the root *is* that lone statement.
inline std::vector<std::shared_ptr<peg::Ast>> collect_top_level_statements(
    const std::shared_ptr<peg::Ast>& ast) {
  using namespace peg::udl;
  if (!ast) return {};
  if (ast->tag == "STATEMENTS"_) return ast->nodes;
  return {ast};
}

// JIT/AOT only: graft the stdlib preamble into the entry module's tree.
//
// The preamble defines helpers (assert_*, Time, …) that user code calls,
// and those bindings must live in the entry module's top-level scope —
// dependency modules compile in isolated scopes, so the preamble can't be
// a separate module. The naive approach (concatenate the preamble onto
// the source text) shifts every user line down by the preamble's height,
// desyncing JIT/AOT error locations and in-language `e.line` from the
// interpreter, which never prepends. Instead we parse the preamble on its
// own and splice its statements ahead of the user's in a fresh STATEMENTS
// root: user nodes keep the exact line/column they were parsed with.
inline void splice_stdlib_preamble(std::vector<LoadedModule>& modules,
                                   std::string_view user_src) {
  if (modules.empty()) return;
  std::string preamble = stdlib_preamble_for(user_src);
  if (preamble.empty()) return;

  // Retain the preamble bytes for the module's lifetime — the spliced
  // AST's tokens are string_views into this buffer.
  auto pre_buf = std::make_shared<std::string>(std::move(preamble));
  std::vector<std::string> msgs;
  auto pre_ast = parse_with_transforms("<stdlib>", pre_buf->data(),
                                       pre_buf->size(), msgs);
  if (!pre_ast) return;  // stdlib is trusted; a parse failure is a build bug

  // Entry module is last (the loader emits dependencies before it).
  LoadedModule& entry = modules.back();
  std::vector<std::shared_ptr<peg::Ast>> stmts =
      collect_top_level_statements(pre_ast);
  for (auto& s : collect_top_level_statements(entry.ast)) {
    stmts.push_back(std::move(s));
  }

  const char* path = entry.ast ? entry.ast->path.c_str() : "";
  auto merged = std::make_shared<peg::Ast>(path, size_t{1}, size_t{1},
                                           "STATEMENTS", stmts);
  for (auto& child : merged->nodes) child->parent = merged;

  entry.ast = std::move(merged);
  entry.aux_sources.push_back(std::move(pre_buf));
}

// Register stdlib modules that should not be parsed/evaluated up front.
// Each module is bound lazily so scripts that never touch it pay zero
// cost. See [[project-startup-overhead]] for the measurement that
// motivated this.
inline void register_stdlib_lazy_modules(Environment& env) {
  env.initialize_lazy("Time", TIME_MODULE_SOURCE);
  env.initialize_lazy("Term", TERM_MODULE_SOURCE);
  env.initialize_lazy("Args", ARGS_MODULE_SOURCE);
  env.initialize_lazy("Regex", REGEX_MODULE_SOURCE);
  env.initialize_lazy("replace", STRING_REPLACE_MODULE_SOURCE);
  env.initialize_lazy("Log", LOG_MODULE_SOURCE);
  // Matcher family — 10 symbols share one source via initialize_lazy_group.
  // First `get` of any matcher parses + evals the source once; the others
  // are picked up by the non-Nil guard in resolve_from_lazy.
  env.initialize_lazy_group(
      {"assert_true", "assert_false", "assert_eq", "assert_ne",
       "assert_lt", "assert_le", "assert_gt", "assert_ge",
       "assert_throws", "assert_close"},
      MATCHERS_MODULE_SOURCE);
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

inline std::shared_ptr<Environment> environment(
    const std::vector<std::string>& argv = {}) {
  auto env = std::make_shared<Environment>();
  setup_core_globals(*env);
  setup_built_in_functions(*env, argv);
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
  // The CLI hoists these two IO members to bare globals before running a
  // script (`install_cli_aliases` in main.cc), so a program sees them
  // without declaring them — the lint must treat them as builtins too.
  names.insert("puts");
  names.insert("print");
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
