#include "toolchain_cmd.h"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// The version, without reaching for culebra.h — which would pull the parser
// and the whole engine into a translation unit that only needs a string. Same
// re-export init_cmd.cc pins the Zed grammar fetch with.
#include <editor_assets_embedded.h>
#include <exe_path.h>  // find_on_path
#include <hash.h>      // culebra::hashing::sha256
#include <os_compat.h>  // os_isatty

#ifdef CULEBRA_INPROCESS_LLD
#include <lld/Common/Driver.h>

#include <llvm/Support/raw_ostream.h>
LLD_HAS_DRIVER(mingw)
#endif

#if defined(CULEBRA_HTTP_ENABLED)
#include <httplib.h>
#endif

namespace fs = std::filesystem;

namespace culebra::toolchain {
namespace {

const std::string_view kVersion = culebra::editor_assets::kCulebraVersion;

// One name for the download, the directory inside it, and the asset on the
// release. Changing it changes all three together.
constexpr std::string_view kKitName = "culebra-toolchain-windows-x64";

// Where releases live. Not configurable: a kit from anywhere else has no
// reason to match this binary's runtime archives, and `install --from` is the
// supported way to use a locally built one.
constexpr std::string_view kReleaseHost = "https://github.com";
constexpr std::string_view kReleasePath = "/yhirose/culebra/releases/download";

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The value of `KEY <value>` in a kit manifest, or empty.
std::string manifest_field(const fs::path& kit, std::string_view key) {
  std::ifstream in(kit / "MANIFEST.txt");
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 &&
        line[key.size()] == ' ')
      return line.substr(key.size() + 1);
  }
  return {};
}

#ifdef _WIN32
// %LOCALAPPDATA% is the per-user, non-roaming place Windows keeps things a
// program installed for itself: no elevation, no registry, and deleting it
// undoes the install completely. Falls back to %USERPROFILE% only so a
// stripped environment gets a diagnosable path rather than a silent cwd write.
fs::path local_app_data() {
  if (const char* p = std::getenv("LOCALAPPDATA"); p && *p) return fs::path(p);
  if (const char* p = std::getenv("USERPROFILE"); p && *p)
    return fs::path(p) / "AppData" / "Local";
  return fs::current_path();
}
#endif

}  // namespace

fs::path kit_dir() {
#ifdef _WIN32
  return local_app_data() / "culebra" / "toolchain" / std::string(kVersion);
#else
  return {};
#endif
}

namespace {

// A kit is usable when it holds the recipe a link splices and says it was
// packed for this version. The version check is the compatibility check: the
// kit's libstdc++ has to be the one the embedded runtime archives were
// compiled against, and only the release that shipped both says so.
bool kit_usable(const fs::path& kit) {
  std::error_code ec;
  if (!fs::exists(kit / "link-recipe.txt", ec)) return false;
  auto v = manifest_field(kit, "VERSION");
  return v == kVersion;
}

}  // namespace

bool link_available() {
#if defined(_WIN32) && defined(CULEBRA_INPROCESS_LLD)
  // The linker is in this binary, so a kit is the only question.
  return kit_usable(kit_dir());
#elif defined(_WIN32)
  // A build without lld linked in still drives an external clang++.
  return !culebra::find_on_path("clang++").empty() &&
         !culebra::find_on_path("ld.lld").empty();
#elif defined(__APPLE__)
  // /usr/bin/cc exists without the Command Line Tools — it is a shim that
  // offers to install them — so its presence proves nothing.
  return std::system("xcode-select -p > /dev/null 2>&1") == 0;
#else
  return !culebra::find_on_path("cc").empty();
#endif
}

std::string missing_toolchain_hint() {
#if defined(_WIN32) && defined(CULEBRA_INPROCESS_LLD)
  return std::format(
      "the link needs the mingw runtime libraries, which Windows does not "
      "ship. culebra can fetch them for version {} (about 6 MB, into\n"
      "  {}\n"
      "which `culebra toolchain uninstall` removes again):\n"
      "    culebra toolchain install",
      kVersion, kit_dir().string());
#elif defined(_WIN32)
  return
      "this build links through an external driver: install mingw-w64's "
      "clang++ and lld (UCRT64) and put C:\\msys64\\ucrt64\\bin on PATH.";
#elif defined(__APPLE__)
  return
      "the link step needs Xcode's Command Line Tools:\n"
      "    culebra toolchain install     (starts Apple's installer)\n"
      "    xcode-select --install        (the same thing, directly)";
#else
  return
      "the link step drives the system `cc` (Debian/Ubuntu: sudo apt install "
      "g++; Fedora: sudo dnf install gcc-c++).";
#endif
}

bool load_recipe(const fs::path& kit, Recipe& out, std::string& err) {
  std::ifstream in(kit / "link-recipe.txt");
  if (!in) {
    err = std::format("no link recipe in {}", kit.string());
    return false;
  }
  auto kits = kit.string();
  std::string line;
  int section = 0;  // 0 header, 1 prefix, 2 suffix
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (line == "--- PREFIX") { section = 1; continue; }
    if (line == "--- SUFFIX") { section = 2; continue; }
    if (line.starts_with("TARGET ")) { out.target = line.substr(7); continue; }
    // @KIT@ is how a kit names its own files without naming the machine that
    // packed it (misc/pack_windows_toolchain.sh checks that nothing else does).
    for (size_t at = line.find("@KIT@"); at != std::string::npos;
         at = line.find("@KIT@", at + kits.size()))
      line.replace(at, 5, kits);
    if (section == 1) out.prefix.push_back(line);
    else if (section == 2) out.suffix.push_back(line);
  }
  if (out.prefix.empty() || out.suffix.empty()) {
    err = std::format("the link recipe in {} is incomplete", kit.string());
    return false;
  }
  return true;
}

#ifdef CULEBRA_INPROCESS_LLD
namespace {

// A compiler driver takes linker options as -Wl,a,b and passes a, b on; the
// linker takes them as plain arguments. That is the whole translation — every
// other token culebra emits is a path or a -l/-L both spell the same way.
void append_driver_arg(std::vector<std::string>& out, std::string_view a) {
  if (!a.starts_with("-Wl,")) { out.emplace_back(a); return; }
  a.remove_prefix(4);
  for (size_t i = 0; i <= a.size();) {
    auto c = a.find(',', i);
    if (c == std::string_view::npos) c = a.size();
    if (c > i) out.emplace_back(a.substr(i, c - i));
    if (c == a.size()) break;
    i = c + 1;
  }
}

}  // namespace

bool link_in_process(const std::vector<std::string>& driver_args,
                     const std::string& output, bool verbose, std::string& err) {
  auto kit = kit_dir();
  Recipe recipe;
  if (!load_recipe(kit, recipe, err)) return false;

  // <toolchain prefix> <culebra's own> -o <out> <toolchain suffix>: the same
  // shape a driver builds, with culebra's tokens where the user object went.
  std::vector<std::string> argv;
  argv.reserve(recipe.prefix.size() + driver_args.size() + recipe.suffix.size() + 3);
  argv.emplace_back("ld.lld");
  for (const auto& a : recipe.prefix) argv.push_back(a);
  for (const auto& a : driver_args) append_driver_arg(argv, a);
  argv.emplace_back("-o");
  argv.push_back(output);
  for (const auto& a : recipe.suffix) argv.push_back(a);

  if (verbose) {
    std::string joined;
    for (const auto& a : argv) { joined += a; joined += ' '; }
    std::println(stderr, "culebra build: link (in-process lld): {}", joined);
  }

  std::vector<const char*> cargv;
  cargv.reserve(argv.size());
  for (const auto& a : argv) cargv.push_back(a.c_str());

  // exitEarly=false: this process has a REPL, a test runner and a build to
  // return to, so the linker must report a failure rather than call exit().
  bool ok = lld::mingw::link(cargv, llvm::outs(), llvm::errs(),
                             /*exitEarly=*/false, /*disableOutput=*/false);
  if (!ok) err = "the linker rejected this program (diagnostics above)";
  return ok;
}
#endif  // CULEBRA_INPROCESS_LLD

namespace {

#ifdef _WIN32

// Extract with the bsdtar Windows has shipped as tar.exe since 10 1803; it
// reads a .zip as readily as a tarball. Writing a zip reader here would add a
// second one to the codebase (init_cmd.cc has the writer) to save a dependency
// every supported Windows already satisfies.
bool extract_zip(const fs::path& archive, const fs::path& into,
                 std::string& err) {
  if (culebra::find_on_path("tar").empty()) {
    err = "tar.exe was not found on PATH (Windows 10 1803+ ships it)";
    return false;
  }
  std::error_code ec;
  fs::create_directories(into, ec);
  auto cmd = std::format("tar -xf \"{}\" -C \"{}\"", archive.string(),
                         into.string());
  if (std::system(cmd.c_str()) != 0) {
    err = std::format("could not extract {}", archive.string());
    return false;
  }
  return true;
}

#if defined(CULEBRA_HTTP_ENABLED)
// GitHub answers a release asset with a redirect to its CDN, so following
// locations is not optional here.
bool http_get(const std::string& host, const std::string& path,
              std::string& body, std::string& err) {
  httplib::Client cli(host);
  cli.set_follow_location(true);
  cli.set_read_timeout(120);
  cli.set_connection_timeout(30);
  if (const char* proxy = std::getenv("HTTPS_PROXY");
      proxy && *proxy) {
    // httplib does not read the environment itself; a corporate network is
    // exactly where a download is most likely to be the blocked step.
    std::string p(proxy);
    auto at = p.rfind("://");
    if (at != std::string::npos) p = p.substr(at + 3);
    auto colon = p.rfind(':');
    if (colon != std::string::npos)
      cli.set_proxy(p.substr(0, colon).c_str(), std::atoi(p.c_str() + colon + 1));
  }
  auto res = cli.Get(path);
  if (!res) {
    err = std::format("could not reach {}{} ({})", host, path,
                      httplib::to_string(res.error()));
    return false;
  }
  if (res->status != 200) {
    err = std::format("{}{} answered {}", host, path, res->status);
    return false;
  }
  body = res->body;
  return true;
}
#endif  // CULEBRA_HTTP_ENABLED

// Put an extracted kit where a link will look for it. Extraction goes to a
// sibling temporary and a rename claims the name, so an interrupted install
// leaves no half-kit for the next `culebra build` to try to link with.
bool place_kit(const fs::path& staged_root, std::string& err) {
  auto inner = staged_root / std::string(kKitName);
  std::error_code ec;
  if (!fs::exists(inner / "link-recipe.txt", ec)) {
    err = std::format("the archive holds no {}/link-recipe.txt", kKitName);
    return false;
  }
  auto v = manifest_field(inner, "VERSION");
  if (v != kVersion) {
    err = std::format(
        "that kit is for culebra {}, this is {} — a kit's libstdc++ has to be "
        "the one this binary's runtime archives were built against",
        v.empty() ? "an unnamed version" : v, kVersion);
    return false;
  }
  // Reinstalling must not be able to leave the user with less than they had.
  // The kit already there is moved aside, not deleted, so a rename or copy
  // that fails can put it back — otherwise a reinstall that goes wrong takes
  // away a working toolchain, which is worse than not reinstalling at all.
  auto dest = kit_dir();
  auto aside = dest.parent_path() / std::format("{}.previous", kVersion);
  fs::create_directories(dest.parent_path(), ec);
  fs::remove_all(aside, ec);
  bool had_one = fs::exists(dest, ec);
  if (had_one) {
    fs::rename(dest, aside, ec);
    if (ec) {
      err = std::format("could not move the installed kit aside: {}",
                        ec.message());
      return false;
    }
  }

  fs::rename(inner, dest, ec);
  if (ec) {
    // Across volumes rename fails; copying is the fallback, not the default,
    // because it is the non-atomic one.
    ec.clear();
    fs::copy(inner, dest, fs::copy_options::recursive, ec);
  }
  if (ec) {
    std::error_code rec;
    fs::remove_all(dest, rec);
    if (had_one) fs::rename(aside, dest, rec);
    err = std::format("could not put the kit at {}: {}{}", dest.string(),
                      ec.message(),
                      had_one ? " (the previous one is still installed)" : "");
    return false;
  }
  fs::remove_all(aside, ec);
  return true;
}

// Check an archive against the `<digest>  <name>` line published beside it.
// One implementation for both sources, so the download's verification is the
// code a `--from` install exercises too — otherwise it would first run on a
// user's machine, at the moment it matters most.
//
// A local archive with no .sha256 beside it is installed unverified: the file
// came from the user, not from the network, and demanding a digest they would
// have to write themselves protects nothing. A .sha256 that IS there is
// always honoured.
bool verify_digest(const fs::path& archive, const fs::path& sums,
                   std::string& err) {
  std::error_code ec;
  if (!fs::exists(sums, ec)) return true;
  auto want = read_file(sums);
  auto sp = want.find_first_of(" \t\r\n");
  auto expect = want.substr(0, sp == std::string::npos ? want.size() : sp);
  if (expect.empty()) {
    err = std::format("{} holds no digest", sums.string());
    return false;
  }
  auto got = culebra::hashing::sha256(read_file(archive));
  if (got != expect) {
    err = std::format("{} does not match its digest\n"
                      "  expected {}\n  got      {}",
                      archive.filename().string(), expect, got);
    return false;
  }
  return true;
}

bool install_windows(const std::string& from, std::string& err) {
  std::error_code ec;
  auto tmp = kit_dir().parent_path() / std::format("staging-{}", kVersion);
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);

  fs::path archive, sums;
  if (!from.empty()) {
    archive = fs::absolute(from);
    if (!fs::exists(archive, ec)) {
      err = std::format("no such file: {}", archive.string());
      return false;
    }
    sums = archive.string() + ".sha256";
    std::println("culebra toolchain: installing from {}", archive.string());
  } else {
#if defined(CULEBRA_HTTP_ENABLED)
    auto asset = std::format("{}/v{}/{}.zip", kReleasePath, kVersion, kKitName);
    auto sumpath = std::format("{}/v{}/{}.zip.sha256", kReleasePath, kVersion,
                               kKitName);
    std::println("culebra toolchain: fetching the Windows kit for culebra {}…",
                 kVersion);
    std::string body, want;
    if (!http_get(std::string(kReleaseHost), asset, body, err)) return false;
    if (!http_get(std::string(kReleaseHost), sumpath, want, err)) return false;
    archive = tmp / std::format("{}.zip", kKitName);
    sums = archive.string() + ".sha256";
    { std::ofstream(archive, std::ios::binary)
          .write(body.data(), std::streamsize(body.size())); }
    { std::ofstream(sums, std::ios::binary)
          .write(want.data(), std::streamsize(want.size())); }
#else
    err = std::format(
        "this build has no HTTP support, so it cannot fetch the kit. Download\n"
        "  {}{}/v{}/{}.zip\n"
        "and pass it to `culebra toolchain install --from <file>`.",
        kReleaseHost, kReleasePath, kVersion, kKitName);
    return false;
#endif
  }

  // The digest says the bytes are intact; the manifest check in place_kit says
  // the kit belongs with this binary. Neither substitutes for the other.
  if (!verify_digest(archive, sums, err)) return false;

  auto staged = tmp / "x";
  if (!extract_zip(archive, staged, err)) return false;
  if (!place_kit(staged, err)) return false;
  fs::remove_all(tmp, ec);
  std::println("culebra toolchain: installed at {}", kit_dir().string());
  return true;
}

#endif  // _WIN32

// Ask, on a terminal, before doing something the user did not type. Both
// streams have to be a terminal: stdin so an answer can arrive, stderr so the
// question is seen (stdout may be a pipe the build is feeding).
bool confirm(std::string_view question) {
  if (!os_isatty(0) || !os_isatty(2)) return false;
  std::print(stderr, "{} [y/N] ", question);
  std::fflush(stderr);
  std::string answer;
  if (!std::getline(std::cin, answer)) return false;
  return answer == "y" || answer == "Y" || answer == "yes";
}

int status() {
  std::println("culebra {}", kVersion);
#ifdef _WIN32
  auto kit = kit_dir();
  std::println("  kit          {}", kit.string());
  if (kit_usable(kit)) {
    Recipe r;
    std::string err;
    load_recipe(kit, r, err);
    std::println("  installed    yes (target {}, packed {})",
                 r.target.empty() ? "?" : r.target,
                 manifest_field(kit, "PACKED"));
  } else if (std::filesystem::exists(kit)) {
    std::println("  installed    no — the directory exists but is not a kit "
                 "for this version");
  } else {
    std::println("  installed    no");
  }
#ifdef CULEBRA_INPROCESS_LLD
  std::println("  linker       lld, carried in this binary");
#else
  std::println("  linker       external clang++ + ld.lld on PATH");
#endif
#elif defined(__APPLE__)
  std::println("  toolchain    Xcode Command Line Tools");
#else
  std::println("  toolchain    the system C++ compiler (`cc`)");
#endif
  // Asked once: on macOS this shells out to xcode-select, and a status line
  // should not run the same probe three times to print one answer.
  bool ready = link_available();
  std::println("  AOT link     {}", ready ? "ready" : "not available");
  if (!ready) std::println("\n{}", missing_toolchain_hint());
  return ready ? 0 : 1;
}

}  // namespace

bool offer_install_interactively() {
  if (link_available()) return true;
#if defined(_WIN32) || defined(__APPLE__)
  if (!confirm("culebra build: no AOT build environment found. Install it?")) {
    std::println(stderr, "culebra build: {}", missing_toolchain_hint());
    return false;
  }
  std::string err;
#ifdef _WIN32
  if (!install_windows("", err)) {
    std::println(stderr, "culebra toolchain: {}", err);
    return false;
  }
  return link_available();
#else
  // Apple's installer is a GUI flow this process cannot wait on, and it
  // reports the same failure for "already installed" as for "cancelled" — so
  // its exit status is not read, and the build does not retry.
  (void)err;
  std::system("xcode-select --install");
  std::println(stderr,
               "culebra build: the Command Line Tools installer is running. "
               "Re-run this build once it finishes.");
  return false;
#endif
#else
  std::println(stderr, "culebra build: {}", missing_toolchain_hint());
  return false;
#endif
}

int main(int argc, char** argv) {
  // The full command line, as every other subcommand takes it: argv[1] is
  // "toolchain" and the verb follows it.
  std::string verb = argc >= 3 ? argv[2] : "status";
  std::string from;
  for (int i = 3; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--from" && i + 1 < argc) { from = argv[++i]; continue; }
    if (a.starts_with("--from=")) { from = a.substr(7); continue; }
    std::println(stderr, "culebra toolchain: unknown argument: {}", a);
    return 2;
  }

  if (verb == "status") return status();

  if (verb == "install") {
#ifdef _WIN32
    if (link_available() && from.empty()) {
      std::println("culebra toolchain: already installed at {}",
                   kit_dir().string());
      return 0;
    }
    std::string err;
    if (!install_windows(from, err)) {
      std::println(stderr, "culebra toolchain: {}", err);
      return 1;
    }
    return 0;
#elif defined(__APPLE__)
    if (!from.empty()) {
      std::println(stderr,
                   "culebra toolchain: --from is a Windows kit; macOS installs "
                   "Apple's Command Line Tools");
      return 2;
    }
    if (link_available()) {
      std::println("culebra toolchain: the Command Line Tools are installed");
      return 0;
    }
    std::system("xcode-select --install");
    std::println("culebra toolchain: Apple's installer is running; re-run "
                 "`culebra build` once it finishes");
    return 0;
#else
    std::println(stderr,
                 "culebra toolchain: installing a C++ toolchain on Linux is "
                 "the package manager's job, and needs root.\n{}",
                 missing_toolchain_hint());
    return 1;
#endif
  }

  if (verb == "uninstall") {
#ifdef _WIN32
    auto kit = kit_dir();
    std::error_code ec;
    // Only ever remove a directory that says it is a kit: this path is built
    // from an environment variable, and a wrong %LOCALAPPDATA% must not turn
    // this into a recursive delete of something else.
    if (!std::filesystem::exists(kit / "MANIFEST.txt", ec)) {
      std::println("culebra toolchain: nothing installed for {}", kVersion);
      return 0;
    }
    std::filesystem::remove_all(kit, ec);
    if (ec) {
      std::println(stderr, "culebra toolchain: could not remove {}: {}",
                   kit.string(), ec.message());
      return 1;
    }
    // Tidy the version-scoped parents, but only while they are empty: another
    // version's kit may be living beside this one.
    std::filesystem::remove(kit.parent_path(), ec);
    std::filesystem::remove(kit.parent_path().parent_path(), ec);
    std::println("culebra toolchain: removed {}", kit.string());
    return 0;
#else
    std::println("culebra toolchain: culebra installed no toolchain here — "
                 "the system's C++ compiler is the package manager's "
                 "(or Xcode's) to remove");
    return 0;
#endif
  }

  std::println(stderr,
               "usage: culebra toolchain [status | install [--from <archive>] "
               "| uninstall]");
  return 2;
}

}  // namespace culebra::toolchain
