#pragma once

// The path of the running executable, in a header that depends on nothing
// else of culebra's. shared.h re-exports it for `Sys.executable` and the
// interpreter; the docs / init command TUs include it directly, which is the
// point — those TUs stay clear of culebra.h and the interpreter stack, so a
// docs-or-init edit never drags the interpreter into their link.

#include <cstring>  // strlen
#include <string>

#if defined(__APPLE__)
#include <climits>        // PATH_MAX
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#include <cstdlib>        // realpath
#elif defined(__linux__)
#include <climits>   // PATH_MAX
#include <unistd.h>  // readlink
#elif defined(_WIN32)
#include <os_compat.h>  // <windows.h> (guarded) — GetModuleFileNameW
#endif

namespace culebra {

// Absolute path to the running culebra executable, for re-spawning a worker
// copy of the interpreter (Sys.executable). macOS: _NSGetExecutablePath;
// Linux: /proc/self/exe. Empty string if it can't be resolved. Backend-neutral
// so interp and JIT both surface the same value.
inline std::string current_executable_path() {
#if defined(__APPLE__)
  uint32_t sz = 0;
  _NSGetExecutablePath(nullptr, &sz);  // first call reports the needed size
  std::string buf(sz, '\0');
  if (_NSGetExecutablePath(buf.data(), &sz) != 0) return "";
  buf.resize(std::strlen(buf.c_str()));
  char real[PATH_MAX];
  if (::realpath(buf.c_str(), real)) return real;
  return buf;
#elif defined(__linux__)
  char buf[PATH_MAX];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return "";
  return std::string(buf, static_cast<size_t>(n));
#elif defined(_WIN32)
  std::wstring wbuf(MAX_PATH, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, wbuf.data(),
                                 static_cast<DWORD>(wbuf.size()));
    if (n == 0) return "";
    if (n < wbuf.size()) {  // fit — n excludes the NUL
      wbuf.resize(n);
      break;
    }
    wbuf.resize(wbuf.size() * 2);  // truncated — grow and retry
  }
  int len = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(),
                                static_cast<int>(wbuf.size()), nullptr, 0,
                                nullptr, nullptr);
  std::string out(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), static_cast<int>(wbuf.size()),
                      out.data(), len, nullptr, nullptr);
  return out;
#else
  return "";
#endif
}

}  // namespace culebra
