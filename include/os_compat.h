#pragma once

// Cross-platform shims for the handful of POSIX calls that appear at scattered
// stdlib call sites (isatty for IO.*_is_terminal, setenv for Env.set) plus a
// Windows fallback for the one signal constant used on a code path that raises
// before it reaches the OS. The larger POSIX subsystems — Proc (proc.h), Term
// (term.h), SharedBuffer (packable.h) and the debug adapter (dap.h) — carry
// their own `#if defined(_WIN32)` split in-file, so this header stays small and
// is the single place the trivial per-call shims live.

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
#include <windows.h>
#include <io.h>       // _isatty
#include <stdlib.h>   // _putenv_s
// SIGKILL has no Windows equivalent; it is passed only to proc::kill_pid on the
// Proc handle-management path, which is a stub that raises "not supported on
// Windows" before the value is ever used. Define it so those call sites compile
// unchanged rather than scattering #if guards through stdlib_interp.h.
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
#if defined(_WIN32)
  return _isatty(fd) != 0;
#else
  return ::isatty(fd) != 0;
#endif
}

// `setenv(3)` — set an environment variable in the current process. On Windows
// `_putenv_s` always overwrites, so the POSIX `overwrite` flag is honoured only
// on the POSIX path (Windows callers pass 1). Returns 0 on success.
inline int os_setenv(const char* name, const char* value, int overwrite) {
#if defined(_WIN32)
  (void)overwrite;
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

// Inverse of gmtime: broken-down UTC time -> time_t.
inline std::time_t os_timegm(std::tm* tm) {
#if defined(_WIN32)
  return _mkgmtime(tm);
#else
  return timegm(tm);
#endif
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
