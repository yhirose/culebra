#include "cli/embed_flags_cmd.h"

#include <filesystem>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "base/source_dir.h"

namespace culebra {
namespace {

// The include path a host build needs. Every entry is load-bearing: the stdlib
// bindings reach stb (font_ttf.h), cpp-regexlib (regex.h), cpp-fstlib (fst.h)
// and both cpp-searchlib roots (search.h, searchlib_segment.h) whether or not
// the script does. docs/deployment.md prints the same list for a reader who
// wants to see it; check_docs_cpp.sh holds the two equal and proves each entry
// is needed.
struct Inc {
  const char* path;
  bool system;  // -isystem: third-party headers whose warnings are not ours
};
constexpr Inc kIncludes[] = {
    {"include", false},
    {"vendor/cpp-peglib", false},
    {"vendor/cpp-unicodelib", false},
    {"vendor/cpp-tensorlib/include", false},
    {"vendor/stb", false},
    {"vendor/cpp-regexlib", false},
    {"vendor/cpp-fstlib", false},
    {"vendor/cpp-searchlib/include", false},
    {"vendor/cpp-searchlib/third_party", true},
};

// One place per platform for what the stdlib reaches at link time, so a host
// asking for flags and an AOT binary link the same set. zlib is Compress and
// Http's gzip; the Apple frameworks are FS.watch (FSEvents) and the Tensor
// backends, none of which are optional in a header-only build.
void append_core_libs(std::vector<std::string>& out) {
  out.emplace_back("-lz");
#ifdef __APPLE__
  out.emplace_back("-framework CoreServices");
  out.emplace_back("-framework Accelerate");
  out.emplace_back("-framework Metal");
#elif defined(__linux__)
  out.emplace_back("-lpthread");
  out.emplace_back("-ldl");
#elif defined(_WIN32)
  out.emplace_back("-lws2_32");   // Net's winsock2, which bindings.h always has
  out.emplace_back("-lpthread");  // winpthread, for isolate's std::thread
#endif
}

void usage(FILE* to) {
  std::println(to,
               R"HELP(Usage: culebra embed-flags (--cflags | --libs | --sources) [feature...]

Print what a C++ host needs to embed culebra (docs/deployment.md §2):

  --cflags           the include path and defines, for the compile step
  --libs             the libraries and frameworks, for the link step
  --sources          culebra .c/.cc files this configuration asks the host
                     to compile, each with its own language's driver (one
                     per line; empty without a feature)

Features (off by default, and absent from the script when off):

  --http             the Http namespace (header-only cpp-httplib)
  --tls              https in Http (implies --http). OpenSSL's own flags are
                     the host's to add: $(pkg-config --cflags openssl) beside
                     --cflags and $(pkg-config --libs openssl) beside --libs
  --sqlite           the SQLite namespace; --sources names the amalgamation
                     TU to compile with it
  --codegen          the CodeGen namespace; --sources names the runtime TU,
                     and the binding is included host-side (deployment.md)
  --jit              the LLVM lane (JIT::run, build_object). Add LLVM's own:
                     -I "$(llvm-config --includedir)" beside --cflags and
                     $(llvm-config --ldflags --libs --system-libs) beside
                     --libs. Not --cxxflags: it carries -std=c++17 and
                     -fno-exceptions, which land on top of culebra's

The flags name a culebra checkout: the tree this binary was built from, or
$CULEBRA_HOME.)HELP");
}

}  // namespace

int run_embed_flags(int argc, const char** argv) {
  bool cflags = false, libs = false, sources = false;
  bool http = false, tls = false, sqlite = false, codegen = false, jit = false;

  for (int i = 2; i < argc; i++) {
    const std::string_view a = argv[i];
    if (a == "--cflags") cflags = true;
    else if (a == "--libs") libs = true;
    else if (a == "--sources") sources = true;
    else if (a == "--http") http = true;
    else if (a == "--tls") tls = http = true;  // TLS is a property of Http
    else if (a == "--sqlite") sqlite = true;
    else if (a == "--codegen") codegen = true;
    else if (a == "--jit") jit = true;
    else if (a == "-h" || a == "--help") { usage(stdout); return 0; }
    else {
      std::println(stderr, "culebra embed-flags: unknown option '{}'", a);
      usage(stderr);
      return 2;
    }
  }
  // One selector, not a mix: the halves go in different places on the
  // command line (libraries after the sources, or a --as-needed link drops
  // them), and --sources is a file list rather than a command line at all.
  if (cflags + libs + sources != 1) {
    std::println(
        stderr,
        "culebra embed-flags: name exactly one of --cflags, --libs, --sources");
    usage(stderr);
    return 2;
  }

  const std::string src_dir = resolved_source_dir();
  // A downloaded binary carries no checkout, and printing flags that point at
  // the machine it was built on would fail later and further away.
  if (src_dir.empty() || !std::filesystem::exists(src_dir + "/include/culebra.h")) {
    std::println(stderr,
                 "culebra embed-flags: no culebra checkout to point at{}",
                 src_dir.empty() ? "" : " (" + src_dir + ")");
    std::println(stderr,
                 "  Embedding builds against the sources. Clone the tree this "
                 "binary's\n  version was built from and set CULEBRA_HOME to "
                 "it.");
    return 1;
  }

  std::vector<std::string> out;
  if (cflags) {
    out.emplace_back("-std=c++23");
    for (const auto& inc : kIncludes)
      out.emplace_back(std::string(inc.system ? "-isystem " : "-I ") + src_dir +
                       "/" + inc.path);
    if (http) {
      out.emplace_back("-DCULEBRA_HTTP_ENABLED");
      out.emplace_back("-I " + src_dir + "/vendor/cpp-httplib");
      if (tls) out.emplace_back("-DCPPHTTPLIB_OPENSSL_SUPPORT");
    }
    if (sqlite) {
      out.emplace_back("-DCULEBRA_SQLITE_ENABLED");
      out.emplace_back("-I " + src_dir + "/vendor/sqlite");
    }
    if (codegen) out.emplace_back("-I " + src_dir + "/vendor/cpp-vmlib");
    if (jit) out.emplace_back("-DCULEBRA_JIT_ENABLED");
  }
  if (libs) {
    append_core_libs(out);
#ifdef __APPLE__
    // OpenSSL's own -L/-l come from the host's pkg-config (its prefix is the
    // host's choice, not ours); these two are cpp-httplib's, for the system
    // trust store it reads through the Security framework
    // (CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN, auto-enabled there).
    if (tls) {
      out.emplace_back("-framework CoreFoundation");
      out.emplace_back("-framework Security");
    }
#endif
  }
  if (sources) {
    // The SQLite amalgamation is compiled with culebra's own options, which
    // live in that TU so a host and this binary run the same SQLite.
    if (sqlite) out.emplace_back(src_dir + "/src/runtime/culebra_sqlite3.c");
    // cpp-vmlib's host-runtime contract, which CodeGen.Module calls into.
    if (codegen) out.emplace_back(src_dir + "/src/runtime/codegen_rt.cc");
  }

  // A file list is one per line; a command line is one line.
  if (sources) {
    for (const auto& f : out) std::println("{}", f);
    return 0;
  }
  std::string line;
  for (const auto& s : out) {
    if (!line.empty()) line += ' ';
    line += s;
  }
  std::println("{}", line);
  return 0;
}

}  // namespace culebra
