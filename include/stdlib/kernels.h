#pragma once

// Engine-neutral native kernels behind the binding layer:
// the compiled lanes (JIT / AOT / VM executor): the glob matcher and FS
// syscall helpers, the File handle side table, the Time calendar/ISO
// helpers, and the Net error mapping. No Value /
// Environment / JitValue here — only plain C++ over shared.h's error type
// and the runtime substate slots — so either engine can include it without
// pulling the other in. Moved verbatim from stdlib_interp.h (Phase 4 B7-b),
// where the compiled lanes used to reach them transitively.

#include <stdlib/net.h>
#include <base/os_compat.h>   // os_strptime (Time), os_* shims
#include <base/shared.h>      // CulebraError, runtime_substate, kSlot*

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#ifdef _WIN32
#include <ext/stdio_filebuf.h>  // libstdc++: FILE* -> streambuf (see _FileStream)
#include <fcntl.h>              // _O_BINARY / _O_APPEND for _open_osfhandle
#endif
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
// and `[...]` character classes (`[!...]` / `[^...]` negate, `a-z` ranges);
// a `[` with no `]` is an ordinary character. Backtracking on `*` — after any
// miss, a class's included. Dir's `glob` default (shared.h) is the same
// matcher in Culebra; the two answer alike.
inline bool match_segment(std::string_view pat, std::string_view name) {
  size_t pi = 0, ni = 0, star = std::string_view::npos, mark = 0;
  while (ni < name.size()) {
    if (pi < pat.size() && pat[pi] == '*') {
      star = pi++;
      mark = ni;
      continue;
    }
    size_t step = 0;  // the pattern characters `name[ni]` matched
    size_t close = pi < pat.size() && pat[pi] == '['
                       ? pat.find(']', pi + 1)
                       : std::string_view::npos;
    if (close != std::string_view::npos) {
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
      if (hit != neg) step = close + 1 - pi;
    } else if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == name[ni])) {
      step = 1;
    }
    if (step) {
      pi += step;
      ++ni;
    } else if (star != std::string_view::npos) {
      pi = star + 1;
      ni = ++mark;
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
  // Segment on whatever this platform calls a separator, and take the root
  // from the same source. Splitting on '/' alone left a Windows pattern —
  // which is what FS.join and Path hand over — as a single segment that
  // matched nothing, and read a drive letter as relative. `path` splits on
  // '\' only where it IS a separator, so POSIX keeps treating it as an
  // ordinary filename character.
  std::filesystem::path pat(pattern);
  std::vector<std::string> segs;
  for (const auto& part : pat.relative_path())
    if (!part.empty()) segs.emplace_back(part.string());  // a trailing sep
  std::vector<std::string> out;
  _glob_detail::expand(pat.root_path(), segs, 0, out);
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
  auto msg = ec ? culebra::format("{}: {}.", what, ec.message())
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
    _io_throw(culebra::format("FS.chown('{}')", p.string()), line, col,
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

// --- Per-user data directory (shared by interp + JIT/AOT/VM) ---
//
// Where a program keeps what outlives the process. The platform decides the
// base (APPDATA / Library/Application Support / XDG_DATA_HOME); the path is
// returned, not created — FS.mkdir makes the parents. `app` must be a single
// path segment, so the result cannot escape the base.
inline std::string _sys_data_dir(std::string_view app, int64_t line,
                                 int64_t col) {
  if (app.empty()) {
    throw CulebraError("ValueError", "Sys.data_dir: app must not be empty",
                       line, col);
  }
  // `:` with the separators: on Windows a name carrying a drive letter
  // replaces the base outright (path::operator/= takes the new root name),
  // so it escapes without ever holding a separator.
  if (app.find_first_of("/\\:") != std::string_view::npos) {
    throw CulebraError(
        "ValueError",
        culebra::format("Sys.data_dir: app must be a single path segment "
                        "(got '{}')",
                        app),
        line, col);
  }
  if (app == "." || app == "..") {
    throw CulebraError("ValueError",
                       "Sys.data_dir: app must not be '.' or '..'", line, col);
  }

  const auto env = [](const char* name) -> const char* {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
  };
  const auto join = [&](std::string_view base, std::string_view tail = "") {
    return (std::filesystem::path(base) / tail / app).string();
  };

#if defined(_WIN32)
  if (const char* appdata = env("APPDATA")) return join(appdata);
  if (const char* profile = env("USERPROFILE"))
    return join(profile, "AppData\\Roaming");
  _io_throw("Sys.data_dir: no data directory (APPDATA and USERPROFILE are "
            "both unset)",
            line, col);
#elif defined(__APPLE__)
  if (const char* home = env("HOME"))
    return join(home, "Library/Application Support");
  _io_throw("Sys.data_dir: no data directory (HOME is unset)", line, col);
#else
  if (const char* xdg = env("XDG_DATA_HOME")) return join(xdg);
  if (const char* home = env("HOME")) return join(home, ".local/share");
  _io_throw("Sys.data_dir: no data directory (XDG_DATA_HOME and HOME are "
            "both unset)",
            line, col);
#endif
}

// --- File handle side table (file-scope, shared between interp + JIT) ---
//
// A File handle is an Object carrying just `_id` (Long); the native
// std::fstream lives here, keyed by id. The table is per-Runtime (Runtime
// is thread_local), so handles don't cross isolate/copy boundaries — which
// matches the concurrency model. close()/drop erase the entry (idempotent:
// a missing id is a no-op or "closed file" error depending on the op).

struct _FileStream {
  // Windows cannot use a plain std::fstream here. That class has no way to
  // ask for FILE_SHARE_DELETE, and without it Windows refuses to remove a
  // file while a handle is open, where POSIX unlinks regardless — so a
  // program that opened a file could not delete it until it closed. Windows
  // opens the HANDLE itself with the share mode it needs and wraps it;
  // everywhere else `stream` is a std::fstream. Every operation below sees
  // only std::iostream, so there is one code path past this point.
  //
  // The wrapper is libstdc++'s stdio_filebuf, which is what makes a FILE*
  // into a streambuf. Windows builds with clang++ but against MinGW's
  // libstdc++, so it is available; moving that lane to libc++ would need a
  // streambuf of our own over the same HANDLE, and nothing outside this
  // struct and _file_open would change.
#ifdef _WIN32
  std::unique_ptr<__gnu_cxx::stdio_filebuf<char>> buf;  // outlives `stream`
#endif
  std::unique_ptr<std::iostream> stream;
  char mode;        // 'r' / 'w' / 'a'
  bool readable;
  bool writable;

  std::iostream& fs() { return *stream; }
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
    _file_throw(culebra::format("File.open: invalid mode '{}' (expected r/w/a)",
                                mode), line, col, "ValueError");
  }
  auto& tbl = _file_table();
  int64_t id = tbl.next_id++;
  auto& slot = tbl.entries[id];
  slot.mode = mode[0];
  slot.readable = readable;
  slot.writable = writable;
  bool opened = false;
#ifdef _WIN32
  // FILE_SHARE_DELETE is the point of opening by hand — see _FileStream.
  // Append asks for FILE_APPEND_DATA rather than GENERIC_WRITE so every
  // write lands at the end even if something else is extending the file,
  // which is what the CRT's "ab" means.
  DWORD access = readable      ? GENERIC_READ
               : slot.mode == 'a' ? FILE_APPEND_DATA
                                  : GENERIC_WRITE;
  DWORD disposition = readable      ? OPEN_EXISTING
                    : slot.mode == 'w' ? CREATE_ALWAYS
                                       : OPEN_ALWAYS;
  HANDLE h = CreateFileW(
      std::filesystem::path(path).wstring().c_str(), access,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    int oflags = _O_BINARY | (readable ? _O_RDONLY : _O_WRONLY) |
                 (slot.mode == 'a' ? _O_APPEND : 0);
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), oflags);
    if (fd == -1) {
      CloseHandle(h);
    } else {
      // The FILE* owns the fd, which owns the HANDLE: closing the filebuf
      // (when this entry is erased) closes all three.
      const char* fmode = readable ? "rb" : (slot.mode == 'a' ? "ab" : "wb");
      if (FILE* f = _fdopen(fd, fmode)) {
        slot.buf = std::make_unique<__gnu_cxx::stdio_filebuf<char>>(f, flags);
        slot.stream = std::make_unique<std::iostream>(slot.buf.get());
        opened = true;
      } else {
        _close(fd);
      }
    }
  }
#else
  auto f = std::make_unique<std::fstream>(path, flags);
  opened = f->is_open();
  if (opened) slot.stream = std::move(f);
#endif
  if (!opened) {
    tbl.entries.erase(id);
    _file_throw(culebra::format("File.open('{}', '{}')", path, mode), line, col);
  }
  return id;
}

// Look up a live stream; IOError if the id was closed.
inline _FileStream& _file_get(int64_t id, const char* op, int64_t line, int64_t col) {
  auto& tbl = _file_table();
  auto it = tbl.entries.find(id);
  if (it == tbl.entries.end()) {
    _file_throw(culebra::format("File.{}: operation on closed file", op), line, col);
  }
  return it->second;
}

inline std::string _file_read_all(int64_t id, int64_t line, int64_t col) {
  auto& s = _file_get(id, "read", line, col);
  if (!s.readable) _file_throw("File.read: file not opened for reading", line, col);
  std::string out((std::istreambuf_iterator<char>(s.fs())),
                  std::istreambuf_iterator<char>());
  return out;
}

inline std::string _file_read_n(int64_t id, int64_t n, int64_t line, int64_t col) {
  auto& s = _file_get(id, "read", line, col);
  if (!s.readable) _file_throw("File.read: file not opened for reading", line, col);
  if (n < 0) n = 0;
  // `n` is a ceiling, not a size to allocate: `read(1 << 40)` on a small file
  // is a small read. The buffer grows with what the file actually yields.
  constexpr size_t kBlock = 64 * 1024;
  std::string buf;
  for (size_t want = static_cast<size_t>(n); want > 0;) {
    const size_t step = std::min(want, kBlock), at = buf.size();
    buf.resize(at + step);
    s.fs().read(buf.data() + at, static_cast<std::streamsize>(step));
    const size_t got = static_cast<size_t>(s.fs().gcount());
    buf.resize(at + got);
    if (got < step) break;  // EOF (or an error the next call reports)
    want -= got;
  }
  return buf;
}

inline void _file_write(int64_t id, std::string_view data, int64_t line, int64_t col) {
  auto& s = _file_get(id, "write", line, col);
  if (!s.writable) _file_throw("File.write: file not opened for writing", line, col);
  s.fs().write(data.data(), static_cast<std::streamsize>(data.size()));
  if (s.fs().bad()) _file_throw("File.write: write failed", line, col);
}

inline void _file_flush(int64_t id, int64_t line, int64_t col) {
  _file_get(id, "flush", line, col).fs().flush();
}

inline void _file_seek(int64_t id, int64_t off, std::string_view whence,
                       int64_t line, int64_t col) {
  auto& s = _file_get(id, "seek", line, col);
  std::ios::seekdir dir;
  if (whence == "set")      dir = std::ios::beg;
  else if (whence == "cur") dir = std::ios::cur;
  else if (whence == "end") dir = std::ios::end;
  else _file_throw(culebra::format("File.seek: invalid whence '{}' "
                                   "(expected set/cur/end)", whence),
                   line, col, "ValueError");
  s.fs().clear();  // clear EOF so seeking past a prior read works
  if (s.readable) s.fs().seekg(off, dir); else s.fs().seekp(off, dir);
}

inline int64_t _file_tell(int64_t id, int64_t line, int64_t col) {
  auto& s = _file_get(id, "tell", line, col);
  return static_cast<int64_t>(s.readable ? s.fs().tellg() : s.fs().tellp());
}

// Read one line (newline stripped, handles \n / \r\n / \r). Returns false
// at end of stream.
inline bool _file_getline(int64_t id, std::string& out, int64_t line, int64_t col) {
  auto& s = _file_get(id, "lines", line, col);
  out.clear();
  int ch;
  bool any = false;
  while ((ch = s.fs().get()) != EOF) {
    any = true;
    if (ch == '\n') return true;
    if (ch == '\r') {
      if (s.fs().peek() == '\n') s.fs().get();
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

inline constexpr int64_t NS_PER_SEC = 1'000'000'000;

// The first calendar field outside its range, and that range — nullopt when
// the fields name one date and time. utc_seconds / mktime would carry month 13 or
// hour 25 into the next unit instead; ISO parsing and `from_parts` refuse it.
struct CivilField {
  const char* name;
  int64_t lo, hi, got;
};
inline std::optional<CivilField> civil_out_of_range(int64_t y, int64_t mo, int64_t d,
                                                    int64_t h, int64_t mi, int64_t se,
                                                    int64_t ns) {
  if (y < 0 || y > 9999) return CivilField{"year", 0, 9999, y};
  if (mo < 1 || mo > 12) return CivilField{"month", 1, 12, mo};
  int64_t dim = days_in_month(static_cast<int>(y), static_cast<int>(mo));
  if (d < 1 || d > dim) return CivilField{"day", 1, dim, d};
  if (h < 0 || h > 23) return CivilField{"hour", 0, 23, h};
  if (mi < 0 || mi > 59) return CivilField{"minute", 0, 59, mi};
  if (se < 0 || se > 59) return CivilField{"second", 0, 59, se};
  if (ns < 0 || ns >= NS_PER_SEC) return CivilField{"nanosecond", 0, NS_PER_SEC - 1, ns};
  return std::nullopt;
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

// Floor-divide nanos into (whole_seconds, sub_seconds_nanos in [0, 1e9)).
// Truncating `%` in C++ misbehaves for negative `nanos`, hence the fixup.
inline std::pair<std::time_t, int64_t> split_nanos(int64_t nanos) {
  auto secs = nanos / NS_PER_SEC;
  auto sub  = nanos % NS_PER_SEC;
  if (sub < 0) { secs -= 1; sub += NS_PER_SEC; }
  return {static_cast<std::time_t>(secs), sub};
}

[[noreturn]] inline void throw_out_of_range() {
  throw_value("Time: out of range (a Long count of nanoseconds, about ±292 years)",
              0, 0);
}

inline int64_t combine_nanos(std::time_t secs, int64_t sub_nanos) {
  int64_t r;
  if (__builtin_mul_overflow(static_cast<int64_t>(secs), NS_PER_SEC, &r) ||
      __builtin_add_overflow(r, sub_nanos, &r))
    throw_out_of_range();
  return r;
}

// UTC in both directions is the proleptic Gregorian calendar computed (Howard
// Hinnant's days_from_civil / civil_from_days), not asked of the platform:
// Windows' _mkgmtime gives up past the year 3000 with -1, which is also a
// valid instant, and its gmtime_s refuses any time before 1970.

// Days since 1970-01-01 of year `y`, month `m` (1..12), day `d` (which may
// run past the month).
inline int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// UTC seconds of a date (month 1..12) and a wall clock, the day and clock
// fields carried past their ranges (hour -25, second 86400) as timegm does.
inline std::time_t utc_seconds(int64_t y, int64_t mo, int64_t d, int64_t h,
                               int64_t mi, int64_t se) {
  return static_cast<std::time_t>(days_from_civil(y, mo, d) * 86400 +
                                  h * 3600 + mi * 60 + se);
}
inline std::time_t utc_seconds(const std::tm& tm) {
  return utc_seconds(int64_t{tm.tm_year} + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// UTC broken-down time of `t`, weekday and day of the year included.
inline std::tm utc_tm(std::time_t t) {
  int64_t s = t;
  int64_t z = (s >= 0 ? s : s - 86399) / 86400;  // whole days, floored
  int64_t sod = s - z * 86400;
  std::tm tm{};
  tm.tm_hour = static_cast<int>(sod / 3600);
  tm.tm_min = static_cast<int>(sod % 3600 / 60);
  tm.tm_sec = static_cast<int>(sod % 60);
  tm.tm_wday = static_cast<int>(((z + 4) % 7 + 7) % 7);  // 1970-01-01: Thu
  int64_t shifted = z + 719468;  // days since 0000-03-01
  int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
  int64_t doe = shifted - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int64_t m = mp < 10 ? mp + 3 : mp - 9;
  int64_t y = yoe + era * 400 + (m <= 2);
  int64_t d = doy - (153 * mp + 2) / 5 + 1;
  tm.tm_year = static_cast<int>(y - 1900);
  tm.tm_mon = static_cast<int>(m - 1);
  tm.tm_mday = static_cast<int>(d);
  tm.tm_yday = static_cast<int>(z - days_from_civil(y, 1, 1));
  return tm;
}

// Local broken-down time of `t`; one the platform cannot represent (Windows
// refuses any time before 1970) is out of range.
inline std::tm local_tm(std::time_t t) {
  std::tm tm{};
  if (!os_localtime_r(&t, &tm)) throw_out_of_range();
  return tm;
}

inline std::tm to_tm(std::time_t t, bool utc) {
  return utc ? utc_tm(t) : local_tm(t);
}

inline std::tm to_tm_nanos(int64_t nanos, bool utc) {
  return to_tm(split_nanos(nanos).first, utc);
}

// Seconds east of UTC of `tm`, the broken-down form of `t`: its wall clock
// read as UTC, less the instant (0 for a UTC one).
inline int64_t utc_offset(const std::tm& tm, std::time_t t) {
  return utc_seconds(tm) - t;
}

inline int64_t from_tm_nanos(std::tm tm, int64_t sub_nanos, bool utc) {
  if (utc) return combine_nanos(utc_seconds(tm), sub_nanos);
  // mktime answers -1 both for a time it cannot represent (on Windows, past
  // the year 3000) and for one second before the epoch; only a success fills
  // in the weekday.
  tm.tm_isdst = -1;
  tm.tm_wday = -1;
  auto t = std::mktime(&tm);
  if (t == -1 && tm.tm_wday == -1) throw_out_of_range();
  return combine_nanos(t, sub_nanos);
}

inline std::optional<int64_t> parse_iso_nanos(std::string_view s) {
  // Sub-second digits accumulate as i64 nanos, up to 9 of them (any past
  // the ninth are discarded).
  if (s.size() < 10) return std::nullopt;
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
  if (civil_out_of_range(y, mo, d, h, mi, se, sub_ns) ||
      std::abs(offset_seconds) >= 24 * 3600)
    return std::nullopt;
  // Date-only / tz-less → treat as UTC (deterministic across hosts).
  std::time_t t = utc_seconds(y, mo, d, h, mi, se);
  if (has_tz) t -= offset_seconds;
  return combine_nanos(t, sub_ns);
}

inline std::string format_iso_nanos(int64_t nanos, bool utc) {
  auto [t, sub] = split_nanos(nanos);
  auto tm = to_tm(t, utc);
  std::string tz_str = "Z";
  if (!utc) {
    auto offset = utc_offset(tm, t);
    int64_t abs_off = offset < 0 ? -offset : offset;
    tz_str = culebra::format("{}{:02d}:{:02d}", offset < 0 ? '-' : '+',
                             static_cast<int>(abs_off / 3600),
                             static_cast<int>((abs_off % 3600) / 60));
  }
  if (sub == 0) {
    return culebra::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}{}",
                           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                           tm.tm_hour, tm.tm_min, tm.tm_sec, tz_str);
  }
  return culebra::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:09d}{}",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec,
                         static_cast<int>(sub), tz_str);
}

inline std::string format_strftime_nanos(int64_t nanos,
                                         const std::string& spec, bool utc) {
  auto t = split_nanos(nanos).first;
  auto tm = to_tm(t, utc);
  // The zone is culebra's to write, not the C library's: on Windows its %z
  // and %Z read the process's local zone whatever `tm` holds. %z is the
  // offset iso() prints, and %Z of a UTC time is "UTC"; only a local zone's
  // name is left to strftime.
  std::string fmt;
  fmt.reserve(spec.size());
  for (size_t i = 0; i < spec.size(); i++) {
    if (spec[i] != '%' || i + 1 == spec.size()) {
      fmt += spec[i];
      continue;
    }
    char c = spec[++i];
    if (c == 'z') {
      auto offset = utc ? 0 : utc_offset(tm, t);
      int64_t abs_off = offset < 0 ? -offset : offset;
      fmt += culebra::format("{}{:02d}{:02d}", offset < 0 ? '-' : '+',
                             static_cast<int>(abs_off / 3600),
                             static_cast<int>((abs_off % 3600) / 60));
    } else if (c == 'Z' && utc) {
      fmt += "UTC";
    } else {
      fmt += '%';
      fmt += c;
    }
  }
  char small[256];
  if (auto n = std::strftime(small, sizeof(small), fmt.c_str(), &tm))
    return std::string(small, n);
  // strftime answers 0 both for a buffer too small and for an empty result,
  // so grow until the output fits or the size no empty result could need.
  for (std::string buf(1024, '\0');; buf.resize(buf.size() * 2)) {
    auto n = std::strftime(buf.data(), buf.size(), fmt.c_str(), &tm);
    if (n > 0 || buf.size() > 16 * (fmt.size() + 16)) {
      buf.resize(n);
      return buf;
    }
  }
}

}  // namespace _time_detail

[[noreturn]] inline void _net_throw(const char* ctx, const std::string& msg,
                                    int64_t line, int64_t col) {
  throw CulebraError("NetError", culebra::format("{}: {}", ctx, msg), line, col);
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

}  // namespace culebra
