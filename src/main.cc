#include <culebra.h>
#include <doctest_runner.h>
#include <stdlib_interp.h>
#include <test_runner.h>
#ifdef CULEBRA_JIT_ENABLED
#include <runtime/aot_scan.h>
#include <stdlib_jit.h>
#include "culebra_rt_assets.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#endif
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <utility>
#include <vector>

using namespace std;

// Startup profiler — gated by CULEBRA_PROFILE_STARTUP=1. Prints each
// phase mark to stderr immediately. See [[project-startup-overhead]].
namespace startup_profile {
using clk = std::chrono::steady_clock;
inline clk::time_point& t0() { static clk::time_point v; return v; }
inline clk::time_point& tprev() { static clk::time_point v; return v; }
inline bool& enabled_flag() { static bool v = false; return v; }
inline void start() {
  const char* e = std::getenv("CULEBRA_PROFILE_STARTUP");
  enabled_flag() = e && *e && e[0] != '0';
  if (enabled_flag()) {
    auto now = clk::now();
    t0() = now;
    tprev() = now;
    std::fprintf(stderr, "[startup-profile] (env CULEBRA_PROFILE_STARTUP set)\n");
  }
}
inline void mark(const char* name) {
  if (!enabled_flag()) return;
  auto t = clk::now();
  auto ms_delta = std::chrono::duration<double, std::milli>(t - tprev()).count();
  auto ms_total = std::chrono::duration<double, std::milli>(t - t0()).count();
  std::fprintf(stderr, "  %7.2f ms (+%6.2f ms)  %s\n", ms_total, ms_delta, name);
  std::fflush(stderr);
  tprev() = t;
}
}  // namespace startup_profile

bool read_file(const char* path, vector<char>& buff) {
  ifstream ifs(path, ios::in | ios::binary);
  if (ifs.fail()) {
    return false;
  }

  auto size = static_cast<unsigned int>(ifs.seekg(0, ios::end).tellg());

  if (size > 0) {
    buff.resize(size);
    ifs.seekg(0, ios::beg).read(&buff[0], static_cast<streamsize>(buff.size()));
  }

  return true;
}

struct Options {
  bool print_ast = false;
  bool shell = false;
  bool debug = false;
  bool help = false;
#ifdef CULEBRA_JIT_ENABLED
  bool jit = false;
  bool emit_llvm = false;
  int opt_level = 2;
#endif
  vector<string> script_path_list;
  vector<string> script_argv;
};

#ifdef CULEBRA_JIT_ENABLED
// Content-fingerprint of the embedded runtime archives, used as a
// cache-directory name so a freshly-built culebra picks up its own
// runtime instead of an older one left behind on disk. FNV-1a 64-bit,
// truncated to 8 hex chars; computed once per process.
static std::string asset_fingerprint() {
  static const std::string cached = []() {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const auto& entry : CulebraRT::FS) {
      if (!entry.is_file()) continue;
      auto data = entry.bytes();
      if (!data) continue;
      for (auto b : *data) {
        h ^= b;
        h *= 0x100000001b3ULL;
      }
    }
    return std::format("{:016x}", h).substr(0, 8);
  }();
  return cached;
}

static std::filesystem::path culebra_cache_dir() {
  // Explicit override wins — useful for CI, sandboxes that can't
  // write under $HOME, or hermetic test runs that want a clean
  // cache per invocation.
  if (const char* c = std::getenv("CULEBRA_CACHE"); c && *c) return c;
  const char* home = std::getenv("HOME");
  if (!home || !*home) {
    const char* tmp = std::getenv("TMPDIR");
    home = (tmp && *tmp) ? tmp : "/tmp";
  }
  return std::filesystem::path(home) / ".cache" / "culebra"
       / asset_fingerprint();
}

// Materialize one embedded runtime archive (by file name) to the on-disk
// cache and return its path. The linker needs a real file, so the driver
// writes the archive once per (culebra binary × machine) and reuses it on
// subsequent invocations.
static std::filesystem::path materialize_archive(
    const std::string& name, std::string& err) {
  auto cache = culebra_cache_dir() / name;
  if (std::filesystem::exists(cache)) return cache;

  auto it = CulebraRT::FS.find(name);
  if (it == CulebraRT::FS.end()) {
    err = std::format("embedded runtime archive '{}' not found", name);
    return {};
  }
  auto data = (*it).bytes();
  if (!data) {
    err = std::format("embedded runtime archive '{}' has no data", name);
    return {};
  }

  std::error_code ec;
  std::filesystem::create_directories(cache.parent_path(), ec);
  if (ec) {
    err = std::format("can't create cache dir '{}': {}",
                      cache.parent_path().string(), ec.message());
    return {};
  }

  // Write to a per-process temp file, then atomically rename into place.
  // Concurrent `culebra build` invocations (e.g. a parallel test run)
  // share this cache; without the rename one process would truncate
  // `cache` while another reads the half-written file and links a corrupt
  // archive. rename(2) is atomic on one filesystem, so a reader sees
  // either no file (and writes its own) or the complete archive.
  auto tmp = cache;
  tmp += std::format(".tmp.{}", ::getpid());
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      err = std::format("can't write '{}'", tmp.string());
      return {};
    }
    out.write(reinterpret_cast<const char*>(data->data()),
              static_cast<std::streamsize>(data->size()));
    if (!out) {
      err = std::format("write failed for '{}'", tmp.string());
      return {};
    }
  }
  std::filesystem::rename(tmp, cache, ec);
  if (ec) {
    // Lost a race (another process renamed first) or cross-device link —
    // if the destination is now a complete archive, use it; else report.
    std::filesystem::remove(tmp);
    if (!std::filesystem::exists(cache)) {
      err = std::format("can't finalize '{}': {}", cache.string(),
                        ec.message());
      return {};
    }
  }
  return cache;
}

struct BuildOptions {
  string input;
  string output;
  string target;     // LLVM target triple; empty = host
  string sysroot;    // forwarded to `cc` as `--sysroot=`
  string rt_lib;     // override runtime archive path (for cross-compile)
  bool emit_llvm = false;
  int opt_level = 2;
};

void print_usage(ostream& os) {
  os << "Usage: culebra [options] [script.cul] [script-args...]\n"
        "       culebra <command> [command-options]\n"
        "\n"
        "Run a culebra program, or start a REPL when no script is given.\n"
        "Arguments after the script path are passed to it as Sys.argv (the\n"
        "Python / Node convention — no `--` needed).\n"
        "\n"
        "Options:\n"
        "  --shell            Start the interactive REPL (the default when\n"
        "                     no script is given)\n"
        "  --ast              Print the parsed AST instead of running it\n"
        "  --debug            Print debug diagnostics while running\n"
#ifdef CULEBRA_JIT_ENABLED
        "  --jit              Run a script through the LLVM JIT instead of the\n"
        "                     tree-walking interpreter (same observable output).\n"
        "                     The REPL always uses the interpreter.\n"
        "  --emit-llvm        Print the generated LLVM IR (with --jit)\n"
        "  -O<level>          JIT optimization level 0-3 (default 2)\n"
#endif
        "  --                 Stop parsing options; the next argument is the\n"
        "                     script even if it begins with '-'\n"
        "  -h, --help         Show this help and exit\n"
        "\n"
        "Commands:\n"
        "  build <in.cul> -o <out>   Compile ahead-of-time into a standalone\n"
        "                            executable (`culebra build --help`)\n"
        "  test [paths...]           Run tests / doctests (--filter, --doc,\n"
        "                            --reporter, --bail, --list)\n"
        "\n"
        "Examples:\n"
        "  culebra hello.cul              Run a script (interpreter)\n"
#ifdef CULEBRA_JIT_ENABLED
        "  culebra --jit hello.cul        Run the same script through the JIT\n"
#endif
        "  culebra                        Start the REPL\n"
        "  culebra app.cul a b c          Run with Sys.argv == [\"a\",\"b\",\"c\"]\n"
        "  culebra build app.cul -o app   Build a standalone binary\n";
}

void print_build_usage(ostream& os) {
  os << "Usage: culebra build <input.cul> -o <output> [options]\n"
        "\n"
        "Compile a culebra program ahead-of-time into a standalone\n"
        "executable.\n"
        "\n"
        "Options:\n"
        "  -o <path>          Output executable path (required)\n"
        "  -O<level>          Optimization level (0-3, default 2)\n"
        "  --emit-llvm        Also write the program's LLVM IR alongside\n"
        "                     the output (for debugging)\n"
        "  --target=<triple>  Cross-compile for the given LLVM target\n"
        "                     triple (e.g. x86_64-unknown-linux-gnu).\n"
        "                     Default: host triple.\n"
        "  --sysroot=<path>   Forward to `cc` as `--sysroot=`. Required\n"
        "                     for most cross-compile targets.\n"
        "  --rt-lib=<path>    Override the runtime archive path. Use to\n"
        "                     point at a libculebra_rt.a built for the\n"
        "                     target. Defaults to the host build.\n";
}

// Triple chars per LLVM convention (alnum + `-_.+`). Strict-enough to
// prevent metacharacters reaching the link command below.
static bool validate_build_triple(const string& v, string& err) {
  if (v.empty()) {
    err = "--target requires a non-empty triple";
    return false;
  }
  for (char c : v) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.' || c == '+';
    if (!ok) {
      err = "--target: invalid character (allowed: alnum, '-', '_', '.', '+')";
      return false;
    }
  }
  return true;
}

// Path values are single-quoted before reaching `cc` via std::system,
// so the validator's job is to reject anything that breaks out of the
// quote (the quote char itself) plus control chars. Other shell
// metacharacters are harmless inside single quotes but rejected
// defensively — none appear in realistic file paths.
static bool validate_build_path(const char* flag, const string& v, string& err) {
  if (v.empty()) {
    err = string(flag) + " requires a non-empty path";
    return false;
  }
  for (unsigned char uc : v) {
    bool bad = uc < 0x20 || uc == 0x7f || uc == '\'' || uc == '"' ||
               uc == '`' || uc == '$' || uc == ';' || uc == '|' ||
               uc == '&' || uc == '<' || uc == '>' || uc == '(' ||
               uc == ')' || uc == '{' || uc == '}' || uc == '!' ||
               uc == '*' || uc == '?' || uc == '\\';
    if (bad) {
      err = string(flag) + ": path contains unsupported character";
      return false;
    }
  }
  return true;
}

bool parse_build_command_line(int argc, const char** argv, BuildOptions& opts,
                              string& err) {
  for (int i = 2; i < argc; ++i) {
    string arg = argv[i];
    if (arg == "-o") {
      if (i + 1 >= argc) {
        err = "-o requires an argument";
        return false;
      }
      opts.output = argv[++i];
    } else if (arg == "--emit-llvm") {
      opts.emit_llvm = true;
    } else if (arg.starts_with("--target=")) {
      opts.target = arg.substr(9);
    } else if (arg.starts_with("--sysroot=")) {
      opts.sysroot = arg.substr(10);
    } else if (arg.starts_with("--rt-lib=")) {
      opts.rt_lib = arg.substr(9);
    } else if (arg.starts_with("-O")) {
      opts.opt_level = std::stoi(arg.substr(2));
    } else if (arg == "-h" || arg == "--help") {
      print_build_usage(cout);
      std::exit(0);
    } else if (arg.starts_with("-")) {
      err = "unknown option: " + arg;
      return false;
    } else {
      if (!opts.input.empty()) {
        err = "multiple input files not supported";
        return false;
      }
      opts.input = arg;
    }
  }

  if (opts.input.empty()) {
    err = "missing input file";
    return false;
  }
  if (opts.output.empty()) {
    err = "missing -o <output>";
    return false;
  }
  if (!validate_build_path("input", opts.input, err)) return false;
  if (!validate_build_path("-o", opts.output, err)) return false;
  if (!opts.target.empty() && !validate_build_triple(opts.target, err))
    return false;
  if (!opts.sysroot.empty() &&
      !validate_build_path("--sysroot", opts.sysroot, err))
    return false;
  if (!opts.rt_lib.empty() &&
      !validate_build_path("--rt-lib", opts.rt_lib, err))
    return false;
  return true;
}

int run_build(const BuildOptions& opts) {
  vector<char> buff;
  if (!read_file(opts.input.c_str(), buff)) {
    std::println(stderr, "culebra build: can't open '{}'", opts.input);
    return 1;
  }
  std::string_view user_src(buff.data(), buff.size());
  vector<string> msgs;
  culebra::ModuleLoader loader;
  std::vector<culebra::LoadedModule> modules;
  try {
    // Parse the user source with its natural line numbers, then splice
    // the stdlib preamble into the entry module's AST. Prepending it as
    // source text would shift every user line and desync error
    // locations from the interpreter (see splice_stdlib_preamble).
    modules = loader.load_program(opts.input, user_src, msgs);
    culebra::splice_stdlib_preamble(modules, user_src);
  } catch (const culebra::CulebraError& e) {
    cerr << e.kind << ": " << e.what();
    if (e.line > 0 || e.col > 0)
      cerr << " at " << e.line << ":" << e.col << ".";
    cerr << endl;
    return 1;
  }
  if (modules.empty()) {
    for (const auto& msg : msgs) cerr << msg << endl;
    return 1;
  }

  auto tmpdir = std::getenv("TMPDIR");
  if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
  auto stem = std::filesystem::path(opts.output).filename().string();
  auto obj = std::format("{}/{}.{}.o", tmpdir, stem,
                         static_cast<unsigned>(::getpid()));

  bool verbose = std::getenv("CULEBRA_VERBOSE") != nullptr;
  if (verbose) std::println(stderr, "culebra build: object -> {}", obj);

  bool cross = !opts.target.empty();

  // Tensor reachability across the whole dependency graph drives both
  // the cross-compile reject and the runtime-archive choice below.
  auto any_uses_tensor = [&]() {
    for (const auto& m : modules) {
      if (culebra::aot_uses_tensor(*m.ast)) return true;
    }
    return false;
  };
  // Http reachability picks the no-http archive (drops OpenSSL + zlib),
  // mirroring the tensor axis.
  auto any_uses_http = [&]() {
    for (const auto& m : modules) {
      if (culebra::aot_uses_http(*m.ast)) return true;
    }
    return false;
  };
  // Compress reachability force-loads the zlib choke object and appends libz,
  // mirroring the tensor/http axes.
  auto any_uses_compress = [&]() {
    for (const auto& m : modules) {
      if (culebra::aot_uses_compress(*m.ast)) return true;
    }
    return false;
  };

  // Reject early: cross-compile + Tensor would need a target-specific
  // BLAS link flag, which Phase E doesn't bundle. Skipping the walk
  // for this case keeps `culebra build --target=...` fast on the
  // failure path.
  if (cross) {
    auto host_triple = llvm::sys::getDefaultTargetTriple();
    if (opts.target == host_triple) {
      cross = false;  // no-op cross is just a host build
    } else if (any_uses_tensor()) {
      std::println(stderr,
          "culebra build: --target=<triple> with Tensor not yet "
          "supported. Drop Tensor references for now.");
      return 1;
    }
  }

  int rc = culebra::JIT::build_object(modules, obj, opts.opt_level,
                                       opts.emit_llvm, opts.target);
  if (rc != 0) return rc;

  bool uses_tensor = cross ? false : any_uses_tensor();
  bool uses_http = cross ? false : any_uses_http();
  bool uses_compress = cross ? false : any_uses_compress();

  // The core archive is feature-axis-free: both heavy deps (cblas via
  // tensor_eval_node, OpenSSL/zlib via http_request) are weak stubs here,
  // so the same archive serves every program. The strong bodies are
  // force-loaded from the per-feature objects below only when the scan
  // reports the feature in use. One core archive, N feature objects — no
  // 2^N variant matrix.
  std::string lib;
  if (!opts.rt_lib.empty()) {
    lib = opts.rt_lib;
  } else if (cross) {
    std::println(stderr,
        "culebra build: --target=<triple> requires --rt-lib=<path>");
    return 1;
  } else {
    std::string err;
    auto path = materialize_archive("libculebra_rt.a", err);
    if (path.empty()) {
      std::println(stderr, "culebra build: {}", err);
      return 1;
    }
    lib = path.string();
  }
  if (!std::filesystem::exists(lib)) {
    std::println(stderr,
        "culebra build: runtime archive not found at '{}'", lib);
    return 1;
  }

  const char* cc = "cc";

  // Link-DCE flag follows the *target* object format. LLVM's Triple
  // knows that "*-apple-*" is Mach-O and everything else is ELF;
  // beats a fragile substring match on the triple string.
  llvm::Triple target_triple_obj(
      cross ? opts.target : llvm::sys::getDefaultTargetTriple());
  bool target_is_macho = target_triple_obj.isOSDarwin();

  const char* dead_strip = target_is_macho ? "-Wl,-dead_strip"
                                           : "-Wl,--gc-sections";

  // A feature object's strong choke (tensor_eval_node / http_request)
  // overrides the core archive's weak stub only if it is actually pulled
  // into the link. A plain `-l`/archive append won't do it — the weak def
  // already satisfies the symbol, so the member is never loaded (verified)
  // — the object must be *force-loaded*: ld64 -force_load / GNU ld
  // --whole-archive. Returns the link fragment, or sets feature_failed + prints.
  bool feature_failed = false;
  auto force_load_feature = [&](const char* archive) -> std::string {
    std::string err;
    auto path = materialize_archive(archive, err);
    if (path.empty()) {
      std::println(stderr, "culebra build: {}", err);
      feature_failed = true;
      return "";
    }
    auto q = std::format("'{}'", path.string());
    return target_is_macho
        ? std::format("-Wl,-force_load,{}", q)
        : std::format("-Wl,--whole-archive {} -Wl,--no-whole-archive", q);
  };

  // Cross builds rely on --rt-lib carrying the feature support, so skip
  // force-loading the embedded feature objects there.
  bool host_build = opts.rt_lib.empty() && !cross;
  std::string tensor_lib =
      (uses_tensor && host_build) ? force_load_feature("libculebra_rt_tensor.a")
                                  : "";
  std::string http_lib =
      (uses_http && host_build) ? force_load_feature("libculebra_rt_http.a")
                                : "";
  std::string compress_lib =
      (uses_compress && host_build)
          ? force_load_feature("libculebra_rt_compress.a")
          : "";
  if (feature_failed) return 1;

  std::string blas = uses_tensor ? CULEBRA_BLAS_LINK : "";
  // OpenSSL (+ zlib, both in CULEBRA_SSL_LINK) is linked only when the program
  // references Http. The cblas/ssl reachability is broken in the core archive
  // by the weak choke stubs (tensor_eval_node / http_request), so a non-Http
  // program references no httplib TLS/zlib symbol and can omit these. The
  // strong http_request is force-loaded via http_lib above when needed, which
  // is what pulls in the symbols these flags resolve. CULEBRA_SSL_LINK is ""
  // when Http is disabled at build time.
  std::string ssl = uses_http ? CULEBRA_SSL_LINK : "";
  // libz: appended when the program references Compress. Http already pulls zlib
  // via CULEBRA_SSL_LINK; a duplicate -lz when both are used is harmless (the
  // linker dedupes). The strong gzip/gunzip are force-loaded via compress_lib
  // above, which is what references the symbols this flag resolves.
  std::string zlib = uses_compress ? CULEBRA_ZLIB_LINK : "";
  std::string libcxx = target_is_macho ? "-lc++" : "-lstdc++ -lm";
  // LLVM's TargetMachine emits a non-PIC object by default. Modern
  // Linux distros (Ubuntu, Fedora) configure their `cc` to link as a
  // PIE executable unconditionally, which then refuses the non-PIC
  // .o with `failed to set dynamic section sizes: bad value`.
  // Force `-no-pie` on non-macOS to match the object's reloc model.
  // (macOS clang/ld take the PIE choice from the .o; no override
  // needed.)
  const char* no_pie = target_is_macho ? "" : "-no-pie";

  // Single-quote every path so $TMPDIR / paths with spaces survive
  // verbatim through std::system. validate_build_path() already rejects
  // the single-quote char itself, so the quoted form can't be escaped.
  auto shq = [](std::string_view s) { return std::format("'{}'", s); };
  std::string extra;
  if (cross) extra += std::format(" --target={}", shq(opts.target));
  if (!opts.sysroot.empty())
    extra += std::format(" --sysroot={}", shq(opts.sysroot));

  std::string cmd = std::format("{}{} {} {} {} {} {} {} {} {} {} {} {} -o {}",
                                cc, extra, shq(obj), shq(lib), tensor_lib,
                                http_lib, compress_lib, dead_strip, no_pie,
                                libcxx, blas, ssl, zlib, shq(opts.output));

  if (verbose) std::println(stderr, "culebra build: link: {}", cmd);
  int link_rc = std::system(cmd.c_str());
  if (link_rc != 0) {
    std::println(stderr, "culebra build: link failed (rc={})", link_rc);
    return 1;
  }

  if (!verbose) std::filesystem::remove(obj);
  return 0;
}
#endif

Options parse_command_line(int argc, const char** argv) {
  Options options;

  // culebra's own flags must precede the script path. The first non-flag
  // argument is THE script; everything after it is passed verbatim to the
  // script as Sys.argv (the Python / Node convention) — no `--` needed.
  // `--` remains an optional escape hatch: it stops flag parsing, so the
  // next argument becomes the script even if it begins with a dash (e.g. a
  // file literally named `-x`).
  int argi = 1;
  bool no_flags = false;     // past `--`: stop interpreting culebra flags
  bool seen_script = false;  // script captured: everything else is its argv
  while (argi < argc) {
    string arg = argv[argi++];
    if (seen_script) {
      options.script_argv.push_back(std::move(arg));
      continue;
    }
    if (!no_flags) {
      if (arg == "--") { no_flags = true; continue; }
      if (arg == "-h" || arg == "--help") { options.help = true; continue; }
      if (arg == "--shell") { options.shell = true; continue; }
      if (arg == "--ast") { options.print_ast = true; continue; }
      if (arg == "--debug") { options.debug = true; continue; }
#ifdef CULEBRA_JIT_ENABLED
      if (arg == "--jit") { options.jit = true; continue; }
      if (arg == "--emit-llvm") { options.emit_llvm = true; continue; }
      if (arg.starts_with("-O")) {
        options.opt_level = std::stoi(arg.substr(2));
        continue;
      }
#endif
    }
    options.script_path_list.push_back(std::move(arg));
    seen_script = true;
  }

  if (!options.shell) {
    options.shell = options.script_path_list.empty();
  }

  return options;
}

bool run_scripts(shared_ptr<culebra::Environment> env, const Options& options) {
  // Time / Args are registered lazily by `environment()` — no eager
  // preamble load needed here. The JIT path still pre-concats the
  // preamble (handled in Phase 3 of [[project-startup-overhead]]).
  startup_profile::mark("run_scripts begin");

  // Cooperative Ctrl+C: install the SIGINT handler and point this thread's
  // Runtime at the global flag. The interpreter's statement poll and the JIT's
  // loop safepoint observe it and throw a catchable `Interrupted`.
  if (!options.script_path_list.empty()) {
    culebra::install_sigint_handler();
  }

  for (auto path : options.script_path_list) {
    vector<char> buff;
    if (!read_file(path.c_str(), buff)) {
      std::println(stderr, "can't open '{}'.", path);
      return false;
    }
    startup_profile::mark("read_file");

    vector<string> msgs;
    std::string_view user_src(buff.data(), buff.size());

    // Walk the dependency graph via ModuleLoader. The same vector
    // feeds both backends — JIT bundles every module into one IR,
    // interp evaluates them sequentially. The JIT path needs the
    // stdlib preamble inlined because it doesn't currently honour
    // the env's lazy bindings (see Phase 3 of [[project-startup-overhead]]).
    // It's spliced into the entry module's AST *after* parse so user line
    // numbers stay natural and error locations match interp (see
    // splice_stdlib_preamble).
    culebra::ModuleLoader loader;
    std::vector<culebra::LoadedModule> modules;
    try {
      modules = loader.load_program(path, user_src, msgs);
#ifdef CULEBRA_JIT_ENABLED
      if (options.jit) culebra::splice_stdlib_preamble(modules, user_src);
#endif
    } catch (const culebra::CulebraError& e) {
      cerr << e.kind << ": " << e.what();
      if (e.line > 0 || e.col > 0) cerr << " at " << e.line << ":" << e.col << ".";
      cerr << endl;
      return false;
    }
    if (modules.empty()) {
      for (const auto& msg : msgs) cerr << msg << endl;
      return false;
    }
    startup_profile::mark("ModuleLoader::load_program (parse)");

    if (options.print_ast) {
      for (const auto& m : modules) {
        cout << "// " << m.abs_path.string() << "\n";
        cout << peg::ast_to_s(m.ast);
      }
    }

#ifdef CULEBRA_JIT_ENABLED
    if (options.jit) {
      culebra::_culebra_sys_argv_holder() = options.script_argv;
      culebra::JIT::run_modules(modules, options.emit_llvm, options.debug,
                                 options.opt_level);
      continue;
    }
#endif

    culebra::Value val;
    auto dbg =
        options.debug ? culebra::CommandLineDebugger() : culebra::Debugger();
    if (culebra::interpret_modules(modules, env, val, msgs, dbg)) {
      startup_profile::mark("interpret_modules");
      continue;
    }
    for (const auto& msg : msgs) cerr << msg << endl;
    return false;
  }

  return true;
}

// The CLI aliases IO.puts and IO.print as globals. Embedders that use
// culebra::environment() directly get a clean environment without
// these global names.
void install_cli_aliases(culebra::Environment& env) {
  const auto& io = env.get("IO").to_object();
  env.initialize("puts", io.get("puts"), false);
  env.initialize("print", io.get("print"), false);
}

int run_test(int argc, const char** argv) {
  std::vector<std::string> roots;
  std::string filter;
  auto reporter = culebra::Reporter::Default;
  int bail_after = 0;
  bool list_only = false;
  bool doc_mode = false;
  auto parse_reporter = [&](std::string_view v) {
    if (v == "default") reporter = culebra::Reporter::Default;
    else if (v == "json") reporter = culebra::Reporter::Json;
    else {
      std::println(stderr, "culebra test: unknown reporter '{}'", v);
      return false;
    }
    return true;
  };
  for (int i = 2; i < argc; i++) {
    std::string arg(argv[i]);
    if (arg.starts_with("--filter=")) {
      filter = arg.substr(9);
    } else if (arg == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    } else if (arg.starts_with("--reporter=")) {
      if (!parse_reporter(arg.substr(11))) return 2;
    } else if (arg == "--reporter" && i + 1 < argc) {
      if (!parse_reporter(argv[++i])) return 2;
    } else if (arg == "--bail") {
      // Optional numeric argument: `--bail` means 1; `--bail 3` means 3.
      if (i + 1 < argc) {
        try {
          bail_after = std::stoi(argv[i + 1]);
          i++;
        } catch (...) {
          bail_after = 1;
        }
      } else {
        bail_after = 1;
      }
    } else if (arg.starts_with("--bail=")) {
      try {
        bail_after = std::stoi(arg.substr(7));
      } catch (...) {
        std::println(stderr, "culebra test: invalid --bail value '{}'",
                      arg.substr(7));
        return 2;
      }
    } else if (arg == "--list") {
      list_only = true;
    } else if (arg == "--doc") {
      doc_mode = true;
    } else if (arg.starts_with("--")) {
      std::println(stderr, "culebra test: unknown option '{}'", arg);
      return 2;
    } else {
      roots.push_back(arg);
    }
  }
  culebra::TestRunSummary summary;
  if (doc_mode) {
    // Doctest mode: extract ` ```culebra ` blocks from Markdown docs and
    // run each in a fresh env. A directory root is walked for `*.md`.
    auto files = culebra::discover_test_files(roots, [](const auto& p) {
      return p.extension() == ".md";
    });
    if (files.empty()) {
      std::println(stderr, "culebra test --doc: no .md files found");
      return 1;
    }
    auto make_env = [] {
      auto e = culebra::environment({});
      install_cli_aliases(*e);
      culebra::install_doctest_exit_guard(*e);
      return e;
    };
    summary = culebra::run_doctests(
        files, filter, make_env, reporter, bail_after, list_only);
  } else {
    auto env = culebra::environment({});
    install_cli_aliases(*env);
    culebra::install_test_ambient(*env);

    auto files = culebra::discover_test_files(roots);
    if (files.empty()) {
      std::println(stderr,
          "culebra test: no test files found (looking for test_*.cul)");
      return 1;
    }
    summary = culebra::run_tests(
        files, filter, env, reporter, bail_after, list_only);
  }

  if (list_only) {
    return summary.errored_files == 0 ? 0 : 1;
  }
  if (reporter == culebra::Reporter::Default) {
    if (summary.errored_files > 0) {
      std::println("{} passed, {} failed, {} file(s) errored",
                    summary.passed, summary.failed, summary.errored_files);
    } else {
      std::println("{} passed, {} failed", summary.passed, summary.failed);
    }
  }
  return (summary.failed == 0 && summary.errored_files == 0) ? 0 : 1;
}

int main(int argc, const char** argv) {
  startup_profile::start();
  startup_profile::mark("main entered");

  // Make the builtin-name set visible to the load-stage undefined-variable
  // lint before any subcommand loads a module (run / build / test all load
  // through the shared module loader, so installing here covers every path).
  culebra::install_undefined_var_lint();

  if (argc >= 2 && string(argv[1]) == "test") {
    return run_test(argc, argv);
  }
#ifdef CULEBRA_JIT_ENABLED
  if (argc >= 2 && string(argv[1]) == "build") {
    culebra::install_jit_stdlib();
    BuildOptions bopts;
    string err;
    if (!parse_build_command_line(argc, argv, bopts, err)) {
      std::println(stderr, "culebra build: {}", err);
      print_build_usage(cerr);
      return 1;
    }
    return run_build(bopts);
  }
#endif

  auto options = parse_command_line(argc, argv);
  startup_profile::mark("parse_command_line");

  if (options.help) {
    print_usage(cout);
    return 0;
  }

#ifdef CULEBRA_JIT_ENABLED
  culebra::install_jit_stdlib();
  startup_profile::mark("install_jit_stdlib");
#endif

  try {
    auto env = culebra::environment(options.script_argv);
    startup_profile::mark("environment() (interp stdlib registered)");
    install_cli_aliases(*env);
    startup_profile::mark("install_cli_aliases");

    if (!run_scripts(env, options)) {
      return -1;
    }

    if (options.shell) {
#ifdef CULEBRA_JIT_ENABLED
      // The REPL always runs on the interpreter (tier 0). A REPL line is never
      // a hot loop, so JIT-compiling each input only adds compile latency for
      // no gain — the same reason V8 / the JVM / LuaJIT start interpreted and
      // only JIT hot code. `--jit` is for scripts (`culebra --jit FILE`), where
      // a hot loop can pay off; combined with the REPL it's a no-op, so note it.
      if (options.jit) {
        std::fprintf(stderr,
            "note: the REPL runs on the interpreter; --jit applies to scripts "
            "(culebra --jit FILE)\n");
      }
#endif
      culebra::repl(env, options.print_ast);
    }
  } catch (const culebra::CulebraError& e) {
    // Uncaught Ctrl+C / cancel: exit with the conventional 128+SIGINT.
    if (e.kind == "Interrupted") {
      cerr << "interrupted" << endl;
      return 130;
    }
    cerr << e.kind << ": " << e.what();
    if (e.line > 0 || e.col > 0) {
      cerr << " at " << e.line << ":" << e.col << ".";
    }
    cerr << endl;
    return -1;
  } catch (const exception& e) {
    cerr << e.what() << endl;
    return -1;
  }

  return 0;
}
