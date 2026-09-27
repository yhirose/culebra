#pragma once

// Cross-platform shims for the handful of POSIX calls that appear at scattered
// stdlib call sites (isatty for IO.*_is_terminal, setenv for Env.set) plus a
// Windows fallback for the one signal constant used on a code path that raises
// before it reaches the OS. The larger POSIX subsystems — Proc (proc.h), Term
// (term.h), SharedBuffer (packable.h) and the debug adapter (dap.h) — carry
// their own `#if defined(_WIN32)` split in-file, so this header stays small and
// is the single place the trivial per-call shims live.

#include <cstdint>
#include <cstdlib>
#include <ctime>
#if defined(_WIN32)
#include <cstring>  // std::strlen (os_strptime end pointer)
#include <iomanip>  // std::get_time (strptime replacement)
#include <sstream>  // std::istringstream
#endif

#if defined(_WIN32)
// Single, safe inclusion point for <windows.h> across the codebase. NOMINMAX
// stops windows.h from defining `min`/`max` macros (which would wreck std::min/
// std::max and numeric_limits::max() used throughout); WIN32_LEAN_AND_MEAN trims
// the rarely-needed sub-headers (winsock is pulled in separately by cpp-httplib).
// Files that need Win32 APIs (shared.h exe path, term.h console size) include
// this header rather than <windows.h> directly, so the guards can't drift.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Target a modern Windows baseline before <windows.h> so version-gated APIs are
// declared. mingw-w64 hides e.g. GetCurrentThreadStackLimits (used by the
// conservative GC's stack_base) behind `_WIN32_WINNT >= 0x0602` (Windows 8), and
// its default can be lower. Windows 10 (0x0A00) is the realistic floor.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
// WIN32_LEAN_AND_MEAN above keeps <windows.h> from pulling this in, and the
// FSCTL_* codes live here — FS.readlink asks FSCTL_GET_REPARSE_POINT for a
// symlink's target.
#include <winioctl.h>

// Windows 10 1703 added the flag that lets an unprivileged process create a
// symlink under Developer Mode. A MinGW headers set older than that compiles
// without it, so name it here rather than at the one call site.
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

// The ceiling the filesystem fixes for a reparse buffer, which FS.readlink
// sizes its read by. ntifs.h names it and a MinGW toolchain does not ship
// that header.
#ifndef MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE 16384
#endif
#include <io.h>       // _isatty
#include <stdlib.h>   // _putenv_s
// SIGKILL has no Windows equivalent; it is passed only to proc::kill_pid on the
// Proc handle-management path, which is a stub that raises "not supported on
// Windows" before the value is ever used. Define it so those call sites compile
// unchanged rather than scattering #if guards through the binding layer.
#ifndef SIGKILL
#define SIGKILL 9
#endif
#else
#include <unistd.h>   // isatty
#endif

namespace culebra {

// `isatty(3)` — true when the fd refers to a terminal. Used by
// IO.*_is_terminal and the Term colour-capability probe.
inline bool os_isatty(int fd) {
#if defined(__EMSCRIPTEN__)
  // No real tty in the browser, so the libc isatty() Emscripten ships
  // always says no. The Playground's "full" (JSPI) build has a working
  // Term.read_key (see term.h), so it reports itself as interactive —
  // otherwise a TUI script's poll loop would spin forever never seeing a
  // key. The "basic" (no-JSPI) build reports non-interactive so the same
  // script takes its non-interactive/piped-input fallback instead, the way
  // it already does natively when stdin isn't a tty.
  (void)fd;
#if defined(CULEBRA_WASM_JSPI)
  return true;
#else
  return false;
#endif
#elif defined(_WIN32)
  return _isatty(fd) != 0;
#else
  return ::isatty(fd) != 0;
#endif
}

// `setenv(3)` — set an environment variable in the current process. `_putenv_s`
// has no no-overwrite mode, so honoring `overwrite == 0` on Windows means
// checking first — a benign race against another thread's own setenv, no
// worse than POSIX setenv's own documented thread-unsafety. Returns 0 on
// success (a skipped set, because the variable was already there, counts as
// success — that is what "don't overwrite" asked for).
inline int os_setenv(const char* name, const char* value, int overwrite) {
#if defined(_WIN32)
  if (!overwrite && std::getenv(name) != nullptr) return 0;
  return _putenv_s(name, value);
#else
  return ::setenv(name, value, overwrite);
#endif
}

// --- Time: POSIX reentrant/parse helpers with Windows equivalents ----------
// The POSIX `*_r` variants and `timegm`/`strptime` have no direct Windows
// spelling (MSVC ships `gmtime_s`/`localtime_s` with swapped arguments,
// `_mkgmtime`, and no strptime). These thin wrappers unify them so the Time
// namespace stays single-sourced across platforms.

// UTC broken-down time. Returns `out` on success, nullptr on failure.
inline std::tm* os_gmtime_r(const std::time_t* t, std::tm* out) {
#if defined(_WIN32)
  return gmtime_s(out, t) == 0 ? out : nullptr;
#else
  return gmtime_r(t, out);
#endif
}

// Local broken-down time. Returns `out` on success, nullptr on failure.
inline std::tm* os_localtime_r(const std::time_t* t, std::tm* out) {
#if defined(_WIN32)
  return localtime_s(out, t) == 0 ? out : nullptr;
#else
  return localtime_r(t, out);
#endif
}

// Inverse of gmtime: broken-down UTC time -> time_t, fields out of their
// range carried as timegm does (month 13, hour -25). Computed from the civil
// calendar (Howard Hinnant's days_from_civil) rather than asked of the
// platform: Windows' _mkgmtime gives up past the year 3000 and answers -1,
// which is also a valid instant. `tm` itself is left as it was.
inline std::time_t os_timegm(const std::tm* tm) {
  int64_t y = int64_t{tm->tm_year} + 1900;
  int64_t m = tm->tm_mon;
  y += m / 12;
  m %= 12;
  if (m < 0) {
    m += 12;
    --y;
  }
  int64_t mm = m + 1;  // 1..12, the year starting in March
  y -= mm <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (mm > 2 ? mm - 3 : mm + 9) + 2) / 5 + tm->tm_mday - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + doe - 719468;
  return static_cast<std::time_t>(days * 86400 + int64_t{tm->tm_hour} * 3600 +
                                  int64_t{tm->tm_min} * 60 + tm->tm_sec);
}

// Parse `s` per strftime-style `fmt` into `*tm`. Returns non-null on success,
// nullptr on mismatch (callers only test null/non-null, not the end pointer).
inline const char* os_strptime(const char* s, const char* fmt, std::tm* tm) {
#if defined(_WIN32)
  std::istringstream in{std::string(s)};
  in >> std::get_time(tm, fmt);
  return in.fail() ? nullptr : s;
#else
  return strptime(s, fmt, tm);
#endif
}

// Seconds east of UTC for a local broken-down time. POSIX carries this on
// `tm_gmtoff`; the Windows `struct tm` has no such field, so reconstruct it —
// interpreting the local wall-clock fields as if they were UTC yields
// `utc + offset`, and subtracting the original time_t leaves the offset.
inline long os_gmtoff(const std::tm& local, std::time_t utc) {
#if defined(_WIN32)
  std::tm copy = local;
  return static_cast<long>(os_timegm(&copy) - utc);
#else
  return static_cast<long>(local.tm_gmtoff);
#endif
}

}  // namespace culebra
