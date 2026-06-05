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

#include <interpreter.h>
#include <proc.h>
#if defined(CULEBRA_HTTP_ENABLED) && !defined(CULEBRA_RT_NO_HTTP)
#include <http.h>
#endif
#include <regexlib.h>

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
#include <unistd.h>  // isatty (IO.*_is_terminal)
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
                            if (!std::getline(std::cin, line)) {
                              return Value(std::string(""));
                            }
                            return Value(std::move(line));
                          },
                          "String"sv)),
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

inline Value make_fs_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  auto throw_io = [](const std::string& what, long line, long col,
                     const std::error_code& ec = {}) {
    auto msg = ec ? std::format("{}: {}.", what, ec.message())
                  : std::string(what);
    throw CulebraError("IOError", std::move(msg), line, col);
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
                              throw_io(std::format("FS.read('{}')", p),
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
              throw_io(std::format("FS.write('{}')", p), line, col);
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

  // `FS.stat(path)` -> Object{size, is_dir, is_file, is_symlink, mtime}.
  // mtime is seconds since the Unix epoch (Long). IOError on missing path.
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
            return Value(std::move(obj));
          },
          "Object"sv)),
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
  const auto& share_v = env->get("share");
  if (share_v.type != Value::Nil) {
    if (share_v.type != Value::Object) {
      throw CulebraError("TypeError",
          std::format("{}: share must be an Object of name -> SharedBuffer",
                      ctx), line, col);
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
      la.overrides.emplace_back(culebra::share_env_key(k), std::move(env_val));
      la.share_fds.push_back(fd);
    }
  }
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

#if defined(CULEBRA_HTTP_ENABLED) && !defined(CULEBRA_RT_NO_HTTP)
// Defined later in this header; forward-declared for the Http helpers' `json:`
// kwarg and `r.json()` method.
inline std::string json_stringify(const Value& v, int indent, bool sort_keys,
                                  int depth);
inline Value json_parse(std::string_view s, std::string_view number_mode);

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

// Configure the request body from the `body` / `json` kwargs (post/put/request).
// `json` (non-nil) serializes the value to JSON and sends it as
// application/json. Otherwise `body`: a String is sent whole; a Function is a
// producer — called per chunk, returning the next chunk String or nil at end —
// streamed chunked so a big upload never lives in memory at once. `body` and
// `json` together is a TypeError; a non-String/Function body is a TypeError.
inline void http_setup_body(const Value& bodyv, const Value& jsonv,
                            const Value& formv, const Value& ct,
                            const std::shared_ptr<Environment>& env,
                            culebra::http::HttpRequest& req, HttpIntoState& st,
                            const char* ctx, long line, long col) {
  bool has_json = jsonv.type != Value::Nil;
  bool has_form = formv.type != Value::Nil;
  bool has_body = bodyv.type == Value::Function ||
                  ((bodyv.type == Value::String ||
                    bodyv.type == Value::StringView) &&
                   !bodyv.to_string_view().empty());
  if (has_json + has_form + has_body > 1) {
    throw CulebraError(
        "TypeError",
        std::format("{}: pass at most one of body, json, form", ctx), line,
        col);
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
         follow_param, into_param, params_param, json_param, form_param},
        [method, ctx](std::shared_ptr<Environment> env) -> Value {
          long line = env->get("__LINE__").to_long();
          long col = env->get("__COLUMN__").to_long();
          culebra::http::HttpRequest req;
          req.method = method;
          req.url = env->get("url").to_string();
          http_fill_common(env, req, ctx, line, col);
          HttpIntoState st;
          http_setup_body(env->get("body"), env->get("json"),
                          env->get("form"), env->get("content_type"), env, req,
                          st, ctx, line, col);
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
           params_param, json_param, form_param},
          [](std::shared_ptr<Environment> env) -> Value {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            culebra::http::HttpRequest req;
            req.method = env->get("method").to_string();
            req.url = env->get("url").to_string();
            http_fill_common(env, req, "Http.request", line, col);
            HttpIntoState st;
            http_setup_body(env->get("body"), env->get("json"),
                            env->get("form"), env->get("content_type"), env, req,
                            st, "Http.request", line, col);
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
            size_t failed = SIZE_MAX;
            auto outcomes = culebra::proc::run_all(
                commands, static_cast<size_t>(lim), nullptr, nullptr, nullptr,
                timeout, fail_fast, &failed, retries);
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
          {{"commands", false, "Array"sv}},
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
            auto [winner, oc] = culebra::proc::run_race(commands);
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

  void advance() {
    if (*p == '\n') { line++; col = 1; }
    else            { col++; }
    ++p;
  }
  void skip_ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
      advance();
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

inline Value json_parse(std::string_view s,
                         std::string_view number_mode = "auto") {
  _JsonParser jp{s.data(), s.data() + s.size()};
  jp.number_mode = number_mode;
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
                               std::string_view number_mode = "auto") {
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
  // `JSON.parse(s, lines=false, number_mode='auto')`:
  //   * lines splits on `\n` and returns an Array, one entry per line.
  //   * number_mode='float' forces every number to Float (round-trip
  //     friendly when the producer treats numbers uniformly).
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
          },
          [](std::shared_ptr<Environment> env) {
            const auto& s = env->get("s").to_string();
            bool lines = env->get("lines").to_bool();
            const auto& mode = env->get("number_mode").to_string();
            if (mode != "auto" && mode != "float") {
              throw CulebraError("ValueError",
                  "JSON.parse: number_mode must be 'auto' or 'float'");
            }
            if (lines) return json_parse_lines(s, mode);
            return json_parse(s, mode);
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
inline Value regex_group_value(const regexlib::Capture& c) {
  using namespace std::literals;
  if (!c.matched) return Value();
  ObjectValue g;
  g.initialize("value", Value(std::string(c.str)), false);
  g.initialize("start", Value(static_cast<long>(c.begin)), false);
  g.initialize("end", Value(static_cast<long>(c.end)), false);
  return Value(std::move(g));
}

// A match -> { value, start, end, groups: [Group|nil], group(i|name) }.
inline Value regex_match_value(const regexlib::MatchResult& m) {
  using namespace std::literals;
  ObjectValue mo;
  mo.initialize("value", Value(std::string(m.str)), false);
  mo.initialize("start", Value(static_cast<long>(m.begin)), false);
  mo.initialize("end", Value(static_cast<long>(m.end)), false);
  ArrayValue groups;
  for (const auto& g : m.groups) groups.values->push_back(regex_group_value(g));
  mo.initialize("groups", Value(std::move(groups)), false);
  // named: { name -> Group } (data only, so it survives the JIT value model too)
  ObjectValue named;
  for (const auto& kv : m.named)
    named.initialize(kv.first, regex_group_value(m.group(kv.second)), false);
  mo.initialize("named", Value(std::move(named)), false);
  return Value(std::move(mo));
}

// Convert a match-time RegexError (e.g. step-budget exceeded) into a
// CulebraError carrying the call site.
inline void regex_rethrow(const regexlib::RegexError& e,
                          const std::shared_ptr<Environment>& env) {
  throw CulebraError("RegexError", std::string(e.what()),
                     env->get("__LINE__").to_long(),
                     env->get("__COLUMN__").to_long());
}

// Compile (or cache-hit) a Regex for `pattern`. The Regex namespace functions
// are stateless — the pattern string is the identity — so a small thread-local
// cache gives reuse without a handle. Flags are written inline ((?i)/(?m)/(?s))
// in the pattern, so the cache key is just the pattern.
inline std::shared_ptr<regexlib::Regex> regex_compile_cached(
    const std::string& pattern, long line, long col) {
  static thread_local std::unordered_map<std::string,
                                          std::shared_ptr<regexlib::Regex>>
      cache;
  auto it = cache.find(pattern);
  if (it != cache.end()) return it->second;
  std::shared_ptr<regexlib::Regex> re;
  try {
    re = std::make_shared<regexlib::Regex>(pattern);
  } catch (const regexlib::RegexError& e) {
    throw CulebraError("RegexError", std::format("Regex: {}", e.what()), line,
                       col);
  }
  if (cache.size() > 256) cache.clear();  // bound growth
  cache.emplace(pattern, re);
  return re;
}

inline std::shared_ptr<regexlib::Regex> regex_from_env(
    const std::shared_ptr<Environment>& env) {
  return regex_compile_cached(std::string(env->get("pattern").to_string()),
                              env->get("__LINE__").to_long(),
                              env->get("__COLUMN__").to_long());
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
                      } catch (const regexlib::RegexError& e) {
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
            return m.matched ? regex_match_value(m) : Value();
          } catch (const regexlib::RegexError& e) {
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
              if (!m.matched) return miss(static_cast<long>(s.size()) + 1);
              long next;
              if (m.end > m.begin) {
                next = pos + static_cast<long>(m.end);
              } else {
                auto seg = regexlib::detail::segment(suffix);
                size_t gi = static_cast<size_t>(m.end_grapheme) + 1;
                size_t nb = gi < seg.byte_begin.size() ? seg.byte_begin[gi]
                                                       : suffix.size() + 1;
                next = pos + static_cast<long>(nb);
              }
              m.begin += pos;
              m.end += pos;
              for (auto& g : m.groups)
                if (g.matched) { g.begin += pos; g.end += pos; }
              out.initialize("m", regex_match_value(m), false);
              out.initialize("nxt", Value(next), false);
              return Value(std::move(out));
            } catch (const regexlib::RegexError& e) {
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
                        for (auto& m : re->find_all(s))
                          av.values->push_back(regex_match_value(m));
                      } catch (const regexlib::RegexError& e) {
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
                        for (auto& m : re->find_all(s))
                          av.values->push_back(Value(std::string(m.str)));
                      } catch (const regexlib::RegexError& e) {
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
                        for (auto& m : re->find_all(s)) {
                          av.values->push_back(Value(static_cast<long>(m.begin)));
                          av.values->push_back(Value(static_cast<long>(m.end)));
                        }
                      } catch (const regexlib::RegexError& e) {
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
                      } catch (const regexlib::RegexError& e) {
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
                      } catch (const regexlib::RegexError& e) {
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
                        for (auto& m : re->find_all(s)) {
                          av.values->push_back(
                              Value(s.substr(cursor, m.begin - cursor)));
                          cursor = m.end;
                        }
                        av.values->push_back(Value(s.substr(cursor)));
                      } catch (const regexlib::RegexError& e) {
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
  env.initialize("_Time", make_time_primitives_namespace(), false);
  env.initialize("Random", make_random_namespace(), false);
  env.initialize("Sys", make_sys_namespace(argv), false);
  env.initialize("GC", make_gc_namespace(), false);
  env.initialize("Tensor", make_tensor_namespace(), false);
  env.initialize("JSON", make_json_namespace(), false);
  env.initialize("Encoding", make_encoding_namespace(), false);
  env.initialize("_Regex", make_regex_primitives_namespace(), false);
  env.initialize("Proc", make_proc_namespace(), false);
#if defined(CULEBRA_HTTP_ENABLED) && !defined(CULEBRA_RT_NO_HTTP)
  env.initialize("Http", make_http_namespace(), false);
#endif
  env.initialize("Isolate", make_isolate_namespace(), false);
  env.initialize("Channel", make_channel_namespace(), false);
  env.initialize("Parallel", make_parallel_namespace(), false);
  env.initialize("SharedBuffer", make_shared_buffer_namespace(), false);
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
inline constexpr const char* TIME_MODULE_SOURCE =
    "let _time_module = fn () { "
    "class Duration { "
    "new(nanos) { this._nanos = nanos } "
    "seconds() { to_float(this._nanos) / 1000000000.0 } "
    "milliseconds() { to_float(this._nanos) / 1000000.0 } "
    "minutes() { this.seconds() / 60.0 } "
    "hours() { this.seconds() / 3600.0 } "
    "days() { this.seconds() / 86400.0 } "
    "abs() { if this._nanos < 0 { Duration.new(-this._nanos) } else { Duration.new(this._nanos) } } "
    "__add__(o) { Duration.new(this._nanos + o._nanos) } "
    "__sub__(o) { Duration.new(this._nanos - o._nanos) } "
    "__mul__(n) { Duration.new(to_long(to_float(this._nanos) * to_float(n))) } "
    "__div__(n) { Duration.new(to_long(to_float(this._nanos) / to_float(n))) } "
    "__neg__() { Duration.new(-this._nanos) } "
    "__lt__(o) { this._nanos < o._nanos } "
    "__le__(o) { this._nanos <= o._nanos } "
    "__eq__(o) { this._nanos == o._nanos } "
    "}; "
    "class Instant { "
    "new(nanos) { this._nanos = nanos } "
    "iso(utc = true) { _Time.iso_nanos(this._nanos, utc) } "
    "format(fmt, utc = false) { _Time.format_nanos(this._nanos, fmt, utc) } "
    "parts(utc = false) { _Time.parts_nanos(this._nanos, utc) } "
    "weekday(utc = false) { _Time.weekday_nanos(this._nanos, utc) } "
    "add(years = 0, months = 0, days = 0, hours = 0, minutes = 0, seconds = 0, utc = false) "
    "{ Instant.new(_Time.add_nanos(this._nanos, years, months, days, hours, minutes, seconds, utc)) } "
    "start_of(unit, utc = false) { Instant.new(_Time.start_of_nanos(this._nanos, unit, utc)) } "
    "unix() { to_float(this._nanos) / 1000000000.0 } "
    "unix_nanos() { this._nanos } "
    "__add__(o) { Instant.new(this._nanos + o._nanos) } "
    "__sub__(o) { match o { d: Duration => Instant.new(this._nanos - d._nanos), "
                          "i: Instant  => Duration.new(this._nanos - i._nanos) } } "
    "__lt__(o) { this._nanos < o._nanos } "
    "__le__(o) { this._nanos <= o._nanos } "
    "__eq__(o) { this._nanos == o._nanos } "
    "}; "
    "{ now: fn() { Instant.new(_Time.now_nanos()) }, "
    "monotonic: fn() { _Time.monotonic() }, "
    "sleep: fn(secs) { _Time.sleep(secs) }, "
    "from_iso: fn(s) { Instant.new(_Time.from_iso_nanos(s)) }, "
    "from_unix: fn(secs) { Instant.new(to_long(to_float(secs) * 1000000000.0)) }, "
    "from_parts: fn(p, utc = false) { Instant.new(_Time.from_parts_nanos(p, utc)) }, "
    "parse: fn(s, fmt) { Instant.new(_Time.parse_nanos(s, fmt)) }, "
    "seconds: fn(n) { Duration.new(to_long(to_float(n) * 1000000000.0)) }, "
    "milliseconds: fn(n) { Duration.new(to_long(to_float(n) * 1000000.0)) }, "
    "minutes: fn(n) { Duration.new(to_long(to_float(n) * 60000000000.0)) }, "
    "hours: fn(n) { Duration.new(to_long(to_float(n) * 3600000000000.0)) }, "
    "days: fn(n) { Duration.new(to_long(to_float(n) * 86400000000000.0)) }, "
    "Instant: Instant, Duration: Duration "
    "} "
    "}; "
    "let Time = _time_module()\n";

// `Args` — declarative CLI argument parser. Spec is a culebra
// Object listing positionals + options + subcommands; `Args.parse`
// returns an Object with parsed fields, prints help on `--help`,
// and reports errors with `Sys.exit(2)`. `try_parse` raises
// `{kind: \"ArgParseError\", message}` for programmatic control.
inline constexpr const char* ARGS_MODULE_SOURCE =
    "let _args_module = fn() { "
    "let _coerce = fn(raw, type, name) { "
    "if type == \"String\" { return raw }; "
    "if type == \"Long\" { return to_long(raw) }; "
    "if type == \"Float\" { return to_float(raw) }; "
    "if type == \"Bool\" { "
    "if raw == \"true\" || raw == \"1\" { return true }; "
    "if raw == \"false\" || raw == \"0\" { return false }; "
    "throw {kind: \"ArgParseError\", "
    "message: \"argument '{name}' expects Bool, got '{raw}'\"} "
    "}; "
    "throw {kind: \"ArgParseError\", "
    "message: \"argument '{name}' has unknown type '{type}'\"} "
    "}; "
    "let _find_by_name = fn(args, name) { "
    "let mut i = 0; "
    "while i < args.size() { "
    "let a = args[i]; "
    "if a.name == name { return a }; "
    "if a.has(\"short\") && a.short == name { return a }; "
    "i += 1 "
    "}; nil "
    "}; "
    "let _is_option = fn(a) { a.has(\"short\") || a.has(\"default\") }; "
    "let _is_positional = fn(a) { !_is_option(a) }; "
    "let _arg_type = fn(a) { if a.has(\"type\") { a.type } else { \"String\" } }; "
    "let _format_help = fn(spec) { "
    "let name = if spec.has(\"name\") { spec.name } else { \"program\" }; "
    "let doc = if spec.has(\"doc\") { spec.doc } else { \"\" }; "
    "let mut parts = []; "
    "if doc != \"\" { parts.push(\"{name} - {doc}\\n\\n\") }; "
    "let mut pos_args = []; "
    "let mut opt_args = []; "
    "let mut i = 0; "
    "while i < spec.args.size() { "
    "let a = spec.args[i]; "
    "if _is_positional(a) { pos_args.push(a) } else { opt_args.push(a) }; "
    "i += 1 "
    "}; "
    "parts.push(\"Usage: {name}\"); "
    "if opt_args.size() > 0 { parts.push(\" [options]\") }; "
    "let mut j = 0; "
    "while j < pos_args.size() { "
    "let a = pos_args[j]; "
    "if a.has(\"default\") { parts.push(\" [<{a.name}>]\") } "
    "else if a.has(\"repeated\") && a.repeated { parts.push(\" <{a.name}>...\") } "
    "else { parts.push(\" <{a.name}>\") }; "
    "j += 1 "
    "}; "
    "parts.push(\"\\n\"); "
    "if pos_args.size() > 0 { "
    "parts.push(\"\\nArguments:\\n\"); "
    "let mut k = 0; "
    "while k < pos_args.size() { "
    "let a = pos_args[k]; "
    "let d = if a.has(\"doc\") { a.doc } else { \"\" }; "
    "parts.push(\"  {a.name}    {d}\\n\"); "
    "k += 1 "
    "} "
    "}; "
    "if opt_args.size() > 0 { "
    "parts.push(\"\\nOptions:\\n\"); "
    "let mut m = 0; "
    "while m < opt_args.size() { "
    "let a = opt_args[m]; "
    "let short = if a.has(\"short\") { \"-{a.short}, \" } else { \"    \" }; "
    "let d = if a.has(\"doc\") { a.doc } else { \"\" }; "
    "parts.push(\"  {short}--{a.name}    {d}\\n\"); "
    "m += 1 "
    "} "
    "}; "
    "parts.push(\"  -h, --help    show this help and exit\\n\"); "
    "parts.join(\"\") "
    "}; "
    "let _route_subcommand = fn(argv, spec) { "
    "if !spec.has(\"subcommands\") { return nil }; "
    "let mut i = 0; "
    "while i < argv.size() { "
    "let tok = argv[i]; "
    "if tok == \"-h\" || tok == \"--help\" { "
    "throw {kind: \"ArgParseHelp\", help: _format_help(spec)} "
    "}; "
    "if tok.starts_with(\"-\") { i += 1; continue }; "
    "let mut j = 0; "
    "while j < spec.subcommands.size() { "
    "let sub = spec.subcommands[j]; "
    "if sub.name == tok { "
    "let mut rest = []; "
    "let mut k = 0; "
    "while k < argv.size() { "
    "if k != i { rest.push(argv[k]) }; "
    "k += 1 "
    "}; "
    "return {sub: sub, argv: rest} "
    "}; "
    "j += 1 "
    "}; "
    "throw {kind: \"ArgParseError\", message: \"unknown subcommand '{tok}'\"} "
    "}; "
    "throw {kind: \"ArgParseError\", message: \"expected subcommand\"} "
    "}; "
    "let _parse_impl_flat = fn(argv, spec) { "
    "let mut result = {}; "
    "let mut positionals = []; "
    "let mut i = 0; "
    "let n = argv.size(); "
    "while i < n { "
    "let tok = argv[i]; "
    "if tok == \"--\" { "
    "let mut j = i + 1; "
    "while j < n { positionals.push(argv[j]); j += 1 }; "
    "i = n "
    "} else if tok == \"-h\" || tok == \"--help\" { "
    "throw {kind: \"ArgParseHelp\", help: _format_help(spec)} "
    "} else if tok.starts_with(\"--\") { "
    "let body = tok.slice(2, tok.size()); "
    "let mut name = body; "
    "let mut explicit_value = nil; "
    "let mut has_value = false; "
    "if body.contains(\"=\") { "
    "let parts = body.split(\"=\"); "
    "name = parts[0]; "
    "let mut v = parts[1]; "
    "let mut pi = 2; "
    "while pi < parts.size() { v = \"{v}={parts[pi]}\"; pi += 1 }; "
    "explicit_value = v; has_value = true "
    "}; "
    "let spec_a = _find_by_name(spec.args, name); "
    "if spec_a == nil { "
    "throw {kind: \"ArgParseError\", message: \"unknown option '--{name}'\"} "
    "}; "
    "if _arg_type(spec_a) == \"Bool\" && !has_value { "
    "result[spec_a.name] = true "
    "} else { "
    "let raw = if has_value { explicit_value } "
    "else { "
    "i = i + 1; "
    "if i >= n { "
    "throw {kind: \"ArgParseError\", "
    "message: \"option '--{name}' expects a value\"} "
    "}; argv[i] "
    "}; "
    "let v = _coerce(raw, _arg_type(spec_a), spec_a.name); "
    "if spec_a.has(\"repeated\") && spec_a.repeated { "
    "if !result.has(spec_a.name) { result[spec_a.name] = [] }; "
    "result[spec_a.name].push(v) "
    "} else { result[spec_a.name] = v } "
    "} "
    "} else if tok.starts_with(\"-\") && tok.size() > 1 { "
    "let body = tok.slice(1, tok.size()); "
    "let mut name = body; "
    "let mut explicit_value = nil; "
    "let mut has_value = false; "
    "if body.contains(\"=\") { "
    "let parts = body.split(\"=\"); "
    "name = parts[0]; "
    "let mut v = parts[1]; "
    "let mut pi = 2; "
    "while pi < parts.size() { v = \"{v}={parts[pi]}\"; pi += 1 }; "
    "explicit_value = v; has_value = true "
    "}; "
    "let spec_a = _find_by_name(spec.args, name); "
    "if spec_a == nil { "
    "throw {kind: \"ArgParseError\", message: \"unknown option '-{name}'\"} "
    "}; "
    "if _arg_type(spec_a) == \"Bool\" && !has_value { "
    "result[spec_a.name] = true "
    "} else { "
    "let raw = if has_value { explicit_value } "
    "else { "
    "i = i + 1; "
    "if i >= n { "
    "throw {kind: \"ArgParseError\", "
    "message: \"option '-{name}' expects a value\"} "
    "}; argv[i] "
    "}; "
    "let v = _coerce(raw, _arg_type(spec_a), spec_a.name); "
    "if spec_a.has(\"repeated\") && spec_a.repeated { "
    "if !result.has(spec_a.name) { result[spec_a.name] = [] }; "
    "result[spec_a.name].push(v) "
    "} else { result[spec_a.name] = v } "
    "} "
    "} else { positionals.push(tok) }; "
    "i += 1 "
    "}; "
    "let mut pos_idx = 0; "
    "let mut spec_idx = 0; "
    "while spec_idx < spec.args.size() { "
    "let a = spec.args[spec_idx]; "
    "if _is_positional(a) { "
    "if a.has(\"repeated\") && a.repeated { "
    "result[a.name] = []; "
    "while pos_idx < positionals.size() { "
    "let v = _coerce(positionals[pos_idx], _arg_type(a), a.name); "
    "result[a.name].push(v); "
    "pos_idx += 1 "
    "} "
    "} else if pos_idx < positionals.size() { "
    "result[a.name] = _coerce(positionals[pos_idx], _arg_type(a), a.name); "
    "pos_idx += 1 "
    "} "
    "}; "
    "spec_idx += 1 "
    "}; "
    "if pos_idx < positionals.size() { "
    "throw {kind: \"ArgParseError\", "
    "message: \"unexpected positional argument '{positionals[pos_idx]}'\"} "
    "}; "
    "let mut k = 0; "
    "while k < spec.args.size() { "
    "let a = spec.args[k]; "
    "if !result.has(a.name) { "
    "if a.has(\"default\") { result[a.name] = a.default } "
    "else if a.has(\"repeated\") && a.repeated { result[a.name] = [] } "
    "else if _arg_type(a) == \"Bool\" { result[a.name] = false } "
    "else { "
    "throw {kind: \"ArgParseError\", "
    "message: \"missing required argument '{a.name}'\"} "
    "} "
    "}; k += 1 "
    "}; "
    "result "
    "}; "
    "let _parse_impl = fn(argv, spec) { "
    "let routed = _route_subcommand(argv, spec); "
    "if routed != nil { "
    "let mut result = _parse_impl_flat(routed.argv, routed.sub); "
    "result.subcommand = routed.sub.name; "
    "return result "
    "}; _parse_impl_flat(argv, spec) "
    "}; "
    "{ "
    "try_parse: fn(argv, spec) { _parse_impl(argv, spec) }, "
    "parse: fn(argv, spec) { try { _parse_impl(argv, spec) } catch e { "
    "if e.has(\"kind\") && e.kind == \"ArgParseHelp\" { "
    "IO.puts(e.help); Sys.exit(0) "
    "}; "
    "IO.print(\"error: \"); "
    "IO.puts(if e.has(\"message\") { e.message } else { e }); "
    "Sys.exit(2) "
    "} }, "
    "help: fn(spec) { _format_help(spec) } "
    "} "
    "}; "
    "let Args = _args_module()\n";

// Matcher family — `assert_true` / `assert_false` / `assert_eq` /
// `assert_ne` / `assert_lt` / `assert_le` / `assert_gt` / `assert_ge` /
// `assert_throws` / `assert_close`. Authored in culebra so all 3
// backends (interp/JIT/AOT) share one implementation: the `==` / `<`
// operators here dispatch through `__eq__` / `__lt__` on class
// instances, matching the operator semantics each backend already
// implements. `assert` itself is not in culebra — production code uses
// `if (!cond) throw {kind: "AssertionError", ...}` (Go / Ruby style).
inline constexpr const char* MATCHERS_MODULE_SOURCE =
    "let assert_true = fn(x) { "
    "if x { return nil }; "
    "throw {kind: \"AssertionError\", "
    "message: \"assert_true failed:\\n  value: {x}\"} "
    "}; "
    "let assert_false = fn(x) { "
    "if !x { return nil }; "
    "throw {kind: \"AssertionError\", "
    "message: \"assert_false failed:\\n  value: {x}\"} "
    "}; "
    "let assert_eq = fn(a, b) { "
    "if a == b { return nil }; "
    "throw {kind: \"AssertionError\", "
    "message: \"assert_eq failed:\\n  left:  {a}\\n  right: {b}\"} "
    "}; "
    "let assert_ne = fn(a, b) { "
    "if a != b { return nil }; "
    "throw {kind: \"AssertionError\", "
    "message: \"assert_ne failed:\\n  left:  {a}\\n  right: {b}\"} "
    "}; "
    "let assert_lt = fn(a, b) { "
    "if a < b { return nil }; "
    "throw {kind: \"AssertionError\", "
    "message: \"assert_lt failed:\\n  left:  {a}\\n  right: {b}\"} "
    "}; "
    "let assert_le = fn(a, b) { "
    "if a <= b { return nil }; "
    "throw {kind: \"AssertionError\", "
    "message: \"assert_le failed:\\n  left:  {a}\\n  right: {b}\"} "
    "}; "
    "let assert_gt = fn(a, b) { "
    "if a > b { return nil }; "
    "throw {kind: \"AssertionError\", "
    "message: \"assert_gt failed:\\n  left:  {a}\\n  right: {b}\"} "
    "}; "
    "let assert_ge = fn(a, b) { "
    "if a >= b { return nil }; "
    "throw {kind: \"AssertionError\", "
    "message: \"assert_ge failed:\\n  left:  {a}\\n  right: {b}\"} "
    "}; "
    "let assert_throws = fn(kind, f) { "
    "if f.params.size() != 0 { "
    "throw {kind: \"ArityError\", "
    "message: \"assert_throws: fn must take 0 parameters (got {f.params.size()})\"} "
    "}; "
    "let mut threw = false; "
    "let mut actual_kind = \"\"; "
    "try { f() } catch e { "
    "threw = true; "
    "actual_kind = if type_of(e) == \"Object\" && e.has(\"kind\") "
    "{ e.kind } else { type_of(e) } "
    "}; "
    "if !threw { "
    "throw {kind: \"AssertionError\", "
    "message: \"assert_throws('{kind}', fn): expected throw but fn returned normally\"} "
    "}; "
    "if actual_kind != kind { "
    "throw {kind: \"AssertionError\", "
    "message: \"assert_throws: expected kind '{kind}' but got '{actual_kind}'\"} "
    "} "
    "}; "
    "let assert_close = fn(a, b, tol) { "
    "let mut diff = a - b; "
    "if diff < 0 { diff = -diff }; "
    "if diff != diff || tol != tol || diff > tol { "
    "throw {kind: \"AssertionError\", "
    "message: \"assert_close failed:\\n  a:    {a}\\n  b:    {b}\\n  diff: {diff} (> tol {tol})\"} "
    "} "
    "}\n";

// `Regex` — the user-facing object API over the native `_Regex` primitives
// (the `_Time` / `Time` split). `Regex.compile(pat, flags?)` returns a Regex
// instance whose methods delegate to `_Regex`; flags ("i"/"m"/"s") are folded
// into the pattern as an inline `(?…)` group. Patterns are best written as
// single-quoted raw strings ('\d+'). A Match is a data object
// { value, start, end, groups: [Group|nil], named: {name: Group} }; no-match nil.
inline constexpr const char* REGEX_MODULE_SOURCE =
    // Lazy match iterator. A class method cannot be a generator (the CPS
    // transform only rewrites top-level `fn` declarations), so `find_iter`
    // delegates here. `_Regex.find_from` returns { m, next } where `next`
    // is the grapheme-correct resume byte, so empty matches still advance.
    "fn _regex_find_iter(pat, s) { "
    "let mut pos = 0; "
    "while pos <= s.size() { "
    "let r = _Regex.find_from(pat, s, pos); "
    "if r.m == nil { return }; "
    "yield r.m; "
    "pos = r.nxt "
    "} }; "
    "let _regex_module = fn () { "
    "class Regex { "
    "new(pattern) { this._pat = pattern; _Regex.check(pattern) } "
    "test(s) { _Regex.test(this._pat, s) } "
    "find(s) { _Regex.find(this._pat, s) } "
    "match(s) { _Regex.match(this._pat, s) } "
    "find_all(s) { _Regex.find_all(this._pat, s) } "
    "find_all_str(s) { _Regex.find_all_str(this._pat, s) } "
    "find_all_index(s) { _Regex.find_all_index(this._pat, s) } "
    "count(s) { _Regex.count(this._pat, s) } "
    "find_iter(s) { _regex_find_iter(this._pat, s) } "
    // String repl -> native $1/$<name> template; Function repl -> call it
    // with each Match and splice the returned String between matches.
    "replace_all(s, repl) { "
    "if type_of(repl) != \"Function\" { return _Regex.replace_all(this._pat, s, repl) }; "
    "let mut out = \"\"; let mut last = 0; "
    "for m in _Regex.find_all(this._pat, s) { out = out + s.slice(last, m.start) + repl(m); last = m.end }; "
    "out + s.slice(last, s.size()) } "
    "split(s) { _Regex.split(this._pat, s) } "
    "}; "
    "{ compile: fn(pattern, flags = \"\") { "
    "Regex.new(if flags == \"\" { pattern } else { \"(?\" + flags + \")\" + pattern }) }, "
    // escape(s): backslash-quote every regex metacharacter so `s` matches
    // literally. The metachar set is a backtick raw string (holds { } \\).
    "escape: fn(s) { "
    "let metas = `\\.^$|?*+()[]{}`; let mut out = \"\"; "
    "for c in s { if metas.contains(c) { out = out + `\\` + c } else { out = out + c } }; "
    "out }, "
    "Regex: Regex } "
    "}; "
    "let Regex = _regex_module()\n";

// Transitional concatenation used by the JIT path until it adopts the
// env's lazy bindings (Phase 3 of [[project-startup-overhead]]). The
// interp path is already preamble-free.
inline const char* _stdlib_preamble_concat() {
  static const std::string s =
      std::string(TIME_MODULE_SOURCE) + ARGS_MODULE_SOURCE +
      MATCHERS_MODULE_SOURCE + REGEX_MODULE_SOURCE;
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
inline std::string stdlib_preamble_for(std::string_view user_src) {
  // Each module is pulled in when its marker appears anywhere in the
  // source; `assert_` pulls the whole matcher family (no other stdlib
  // symbol starts with it). Adding a module is a single row here.
  static constexpr struct {
    std::string_view marker;
    const char* source;
  } kModules[] = {
      {"Time", TIME_MODULE_SOURCE},
      {"Args", ARGS_MODULE_SOURCE},
      {"assert_", MATCHERS_MODULE_SOURCE},
      {"Regex", REGEX_MODULE_SOURCE},
  };
  std::string preamble;
  for (const auto& m : kModules) {
    if (user_src.find(m.marker) != std::string_view::npos)
      preamble.append(m.source);
  }
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
  env.initialize_lazy("Args", ARGS_MODULE_SOURCE);
  env.initialize_lazy("Regex", REGEX_MODULE_SOURCE);
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

}  // namespace culebra

// Isolate / Channel stdlib + the sendable value-transfer layer. Included last
// because it depends on the full stdlib defined above (environment(),
// Interpreter). Provides the definition of make_isolate_namespace() declared
// near setup_built_in_functions.
#include "isolate.h"
