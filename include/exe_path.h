#pragma once

// Where a program is: the running executable, and any name on PATH. A header
// that depends on nothing else of culebra's. shared.h re-exports it for
// `Sys.executable` and the interpreter; the docs / init command TUs include it
// directly, which is the point — those TUs stay clear of culebra.h and the
// interpreter stack, so a docs-or-init edit never drags the interpreter into
// their link.

#include <cstdlib>  // getenv
#include <cstring>  // strlen
#include <filesystem>
#include <string>
#include <string_view>

#if defined(__APPLE__)
#include <climits>        // PATH_MAX
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#elif defined(__linux__)
#include <climits>   // PATH_MAX
#include <unistd.h>  // readlink
#elif defined(_WIN32)
#include <os_compat.h>  // <windows.h> (guarded) — GetModuleFileNameW
#endif

namespace culebra {

// Absolute path to the running culebra executable, for re-spawning a worker
// copy of the interpreter (Sys.executable). macOS: _NSGetExecutablePath;
// Linux: /proc/self/exe. Empty string if it can't be resolved.
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

// Absolute path to `name` as PATH would resolve it, or "" if it isn't there.
// A plain string scan rather than a shell-out to `command -v`/`where`: the
// callers ask about a handful of fixed names (an editor CLI, the link
// driver), which isn't worth proc.h's subprocess machinery. On Windows the
// name carries no extension — the launchable spellings are tried here, so a
// toolchain installed as a `.cmd` shim is found too.
inline std::string find_on_path(std::string_view name) {
  namespace fs = std::filesystem;
#if defined(_WIN32)
  constexpr char kPathSep = ';';
#else
  constexpr char kPathSep = ':';
#endif
  const char* path_env = std::getenv("PATH");
  if (!path_env) return "";
  std::string_view path(path_env);
  for (size_t pos = 0; pos <= path.size();) {
    size_t sep = path.find(kPathSep, pos);
    std::string_view dir = path.substr(
        pos, sep == std::string_view::npos ? sep : sep - pos);
    if (!dir.empty()) {
      fs::path base = fs::path(dir) / name;
      std::error_code ec;
#if defined(_WIN32)
      for (const char* ext : {".exe", ".cmd", ".bat", ""}) {
        fs::path candidate = base;
        candidate += ext;
        if (fs::exists(candidate, ec)) return candidate.string();
      }
#else
      if (fs::exists(base, ec)) return base.string();
#endif
    }
    if (sep == std::string_view::npos) break;
    pos = sep + 1;
  }
  return "";
}

}  // namespace culebra
