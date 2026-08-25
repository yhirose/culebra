#pragma once

// Engine-neutral native kernels behind the binding layer:
// the compiled lanes (JIT / AOT / VM executor): the glob matcher and FS
// syscall helpers, the File handle side table, the Time calendar/ISO
// helpers, the Net error mapping, and the Regex compile cache. No Value /
// Environment / JitValue here — only plain C++ over shared.h's error type
// and the runtime substate slots — so either engine can include it without
// pulling the other in. Moved verbatim from stdlib_interp.h (Phase 4 B7-b),
// where the compiled lanes used to reach them transitively.

#include <net.h>
#include <os_compat.h>   // os_strptime (Time), os_* shims
#include <regexlib.h>
#include <shared.h>      // CulebraError, runtime_substate, kSlot*

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
#include <grp.h>       // getgrnam_r (FS.chown group-name resolution)
#include <pwd.h>       // getpwnam_r (FS.chown user-name resolution)
#include <sys/stat.h>  // ::stat for st_uid/st_gid (FS.stat owner fields)
#include <unistd.h>    // chown (FS.chown)
#endif

namespace culebra {

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

}  // namespace culebra
