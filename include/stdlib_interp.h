#pragma once

// Interpreter-side implementation of the Culebra standard library.
//
// Core built-ins bound on every environment: assert, to_long,
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

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <thread>
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

  ns.initialize(
      "min",
      Value(FunctionValue({}, [numeric_reduce](std::shared_ptr<Environment> env) {
        return numeric_reduce(env, [](double a, double b) { return a < b; });
      })),
      false);

  ns.initialize(
      "max",
      Value(FunctionValue({}, [numeric_reduce](std::shared_ptr<Environment> env) {
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
                                      std::cout
                                          << str_quoted_with_special(
                                                 env->get("arg"))
                                          << std::endl;
                                      return Value();
                                    })),
                false);

  ns.initialize("print",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      std::cout << str_display_with_special(
                                          env->get("arg"));
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

  ns.initialize(
      "read",
      Value(FunctionValue({{"path", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& p = env->get("path").to_string();
                            std::ifstream ifs(p, std::ios::binary);
                            if (!ifs) {
                              auto line = env->get("__LINE__").to_long();
                              auto col = env->get("__COLUMN__").to_long();
                              throw CulebraError("IOError",
                                  std::format("IO.read: cannot open '{}' at {}:{}.",
                                              p, line, col),
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
          [](std::shared_ptr<Environment> env) {
            const auto& p = env->get("path").to_string();
            const auto& c = env->get("content").to_string();
            std::ofstream ofs(p, std::ios::binary);
            if (!ofs) {
              auto line = env->get("__LINE__").to_long();
              auto col = env->get("__COLUMN__").to_long();
              throw CulebraError("IOError",
                  std::format("IO.write: cannot open '{}' at {}:{}.",
                              p, line, col),
                  line, col);
            }
            ofs.write(c.data(), c.size());
            return Value();
          })),
      false);

  ns.initialize(
      "exists",
      Value(FunctionValue({{"path", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& p = env->get("path").to_string();
                            std::error_code ec;
                            return Value(std::filesystem::exists(p, ec));
                          },
                          "Bool"sv)),
      false);

  return Value(std::move(ns));
}

inline Value make_fs_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  auto throw_io = [](const std::string& what, long line, long col,
                     const std::error_code& ec = {}) {
    auto msg = ec ? std::format("{} at {}:{}: {}.", what, line, col, ec.message())
                  : std::format("{} at {}:{}.", what, line, col);
    throw CulebraError("IOError", std::move(msg), line, col);
  };

  // `FS.join(parts...)` — varargs path concat. Empty arg list -> "".
  ns.initialize(
      "join",
      Value(FunctionValue({}, [throw_io](std::shared_ptr<Environment> env) {
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
          {{"path", false, "String"sv}},
          [throw_io](std::shared_ptr<Environment> env) {
            long line = env->get("__LINE__").to_long();
            long col = env->get("__COLUMN__").to_long();
            const auto& p = env->get("path").to_string();
            std::error_code ec;
            if (!std::filesystem::remove(p, ec) || ec) {
              throw_io(std::format("FS.remove('{}')", p), line, col, ec);
            }
            return Value();
          })),
      false);

  return Value(std::move(ns));
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
  throw CulebraError("ValueError",
                     std::format("{} at {}:{}.", msg, line, col),
                     line, col);
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

  // Variadic ctor: declared with no formal params so every positional
  // arg lands in __ARGS__. Layout is [optional dtype string, shape
  // varargs OR single Array of Long].
  auto make_ctor = [](TensorPtr (*kernel)(TensorShape, Dtype)) {
    return Value(FunctionValue(
        {},
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

  // Activations: namespace functions (not Tensor methods), since
  // `relu / sigmoid / softmax` are common class-method names — having
  // them as methods would shadow user definitions. The namespace form
  // makes the call site explicitly Tensor-flavored.
  auto make_unary_ns = [](Op op) {
    return Value(FunctionValue(
        {{"t", false, "Tensor"sv}},
        [op](std::shared_ptr<Environment> env) {
          const auto& t = env->get("t").to_tensor().impl;
          return Value(TensorValue(tensor_unary(op, t)));
        },
        "Tensor"sv));
  };
  ns.initialize("sigmoid", make_unary_ns(Op::Sigmoid), false);
  ns.initialize("relu", make_unary_ns(Op::Relu), false);
  ns.initialize("softmax", make_unary_ns(Op::Softmax), false);

  ns.initialize(
      "eval",
      Value(FunctionValue(
          {},
          [](std::shared_ptr<Environment> env) {
            auto line = env->get("__LINE__").to_long();
            auto col = env->get("__COLUMN__").to_long();
            if (!env->has("__ARGS__")) return Value();
            const auto& args = *env->get("__ARGS__").to_array().values;
            for (const auto& v : args) {
              if (v.type != Value::Tensor) throw_type_error_at(line, col);
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
                       std::format("JSON.parse: {} at {}:{}.", msg,
                                   line, col),
                       line, col);
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
                       std::format("JSON.parse: trailing characters at {}:{}.",
                                   jp.line, jp.col),
                       jp.line, jp.col);
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
                           std::format("JSON.parse(lines: true): trailing "
                                       "characters at {}:{}.",
                                       jp.line, jp.col),
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
              {"indent", false, ""sv, nullptr, kw_default_zero()},
              {"sort_keys", false, ""sv, nullptr, kw_default_false()},
              {"lines", false, ""sv, nullptr, kw_default_false()},
          },
          [](std::shared_ptr<Environment> env) {
            const auto& v = env->get("v");
            auto indent_v = env->get("indent");
            if (indent_v.type != Value::Long) {
              throw CulebraError("TypeError",
                  "JSON.stringify: indent must be Long");
            }
            int indent = static_cast<int>(indent_v.get<long>());
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
              {"lines", false, ""sv, nullptr, kw_default_false()},
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

inline void setup_built_in_functions(
    Environment& env, const std::vector<std::string>& argv = {}) {
  using namespace std::literals;

  env.initialize(
      "assert",
      Value(FunctionValue({{"arg", true}},
                          [](std::shared_ptr<Environment> env) {
                            auto cond = env->get("arg").to_bool();
                            if (!cond) {
                              auto line = env->get("__LINE__").to_long();
                              auto column = env->get("__COLUMN__").to_long();
                              throw CulebraError("AssertionError",
                                  std::format("assert failed at {}:{}.", line, column),
                                  line, column);
                            }
                            return Value();
                          })),
      false);

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
  env.initialize("_Time", make_time_primitives_namespace(), false);
  env.initialize("Random", make_random_namespace(), false);
  env.initialize("Sys", make_sys_namespace(argv), false);
  env.initialize("Tensor", make_tensor_namespace(), false);
  env.initialize("JSON", make_json_namespace(), false);
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

// Transitional concatenation used by the JIT path until it adopts the
// env's lazy bindings (Phase 3 of [[project-startup-overhead]]). The
// interp path is already preamble-free.
inline const char* _stdlib_preamble_concat() {
  static const std::string s =
      std::string(TIME_MODULE_SOURCE) + ARGS_MODULE_SOURCE;
  return s.c_str();
}
inline const char* STDLIB_PREAMBLE_SOURCE = _stdlib_preamble_concat();

// Concatenate `user_src` with the stdlib modules it appears to
// reference (substring match). Used by the JIT and AOT paths to skip
// Time/Args parsing when scripts don't touch them. A substring hit is
// a safe over-approximation: false positives (e.g. `let myTime = 1`)
// just include an unneeded module; only true negatives skip the
// prepend, preserving correctness.
inline std::string prepend_stdlib_preamble_selective(
    std::string_view user_src) {
  bool needs_time = user_src.find("Time") != std::string_view::npos;
  bool needs_args = user_src.find("Args") != std::string_view::npos;
  std::string combined;
  combined.reserve(
      (needs_time ? std::strlen(TIME_MODULE_SOURCE) : 0) +
      (needs_args ? std::strlen(ARGS_MODULE_SOURCE) : 0) + user_src.size());
  if (needs_time) combined.append(TIME_MODULE_SOURCE);
  if (needs_args) combined.append(ARGS_MODULE_SOURCE);
  combined.append(user_src);
  return combined;
}

// Register stdlib modules that should not be parsed/evaluated up front.
// Each module is bound lazily so scripts that never touch it pay zero
// cost. See [[project-startup-overhead]] for the measurement that
// motivated this.
inline void register_stdlib_lazy_modules(Environment& env) {
  env.initialize_lazy("Time", TIME_MODULE_SOURCE);
  env.initialize_lazy("Args", ARGS_MODULE_SOURCE);
}

inline std::shared_ptr<Environment> environment(
    const std::vector<std::string>& argv = {}) {
  auto env = std::make_shared<Environment>();
  setup_core_globals(*env);
  setup_built_in_functions(*env, argv);
  register_stdlib_lazy_modules(*env);
  return env;
}

}  // namespace culebra
