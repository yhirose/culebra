#include <culebra.h>
#include <dap.h>
#include <doctest_runner.h>
#include <formatter.h>
#include <stdlib_interp.h>
#include <test_runner.h>
#ifdef CULEBRA_JIT_ENABLED
#ifndef CULEBRA_WRAP_LINK_FLAGS
#define CULEBRA_WRAP_LINK_FLAGS ""
#endif
#include <runtime/aot_scan.h>
#include <stdlib_jit.h>
#include "culebra_rt_assets.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#endif
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <utility>
#include <vector>

using namespace std;

// --- Link anchor: force out-of-line emission of the Shared.new view readers.
// interpreter.h forward-declares shared_val_get_prop / _get_index / _make_iter
// (non-inline) and calls them from eval_property; their inline definitions
// live in sharedval.h, reached here via stdlib_interp.h -> isolate.h. A
// `culebra wrap` extension TU sees only the forward declarations (wrap.h is
// included before `environment()` exists, so it cannot pull isolate.h), so its
// out-of-line eval_property copy leaves an undefined reference on linkers that
// don't fold it (Linux/mold). Taking their addresses in this TU — which DOES
// have the definitions and is always in the driver/wrap link — forces a weak
// out-of-line copy the wrap link resolves against.
namespace culebra {
[[gnu::used]] void* const _shared_val_reader_anchor[] = {
    reinterpret_cast<void*>(&shared_val_get_prop),
    reinterpret_cast<void*>(&shared_val_get_index),
    reinterpret_cast<void*>(&shared_val_make_iter),
};
}  // namespace culebra

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
  // FastISel backend: ~halves JIT warmup (startup/compile time) at a small
  // steady-state cost (~7% on pure-script hot loops; ~0% when the hot work
  // lives in the C++/BLAS runtime, e.g. Tensor). Output is interp-symmetric
  // (the whole difftest corpus passes under --jit-faststart). Opt-in: the
  // default --jit (O2) keeps the best steady-state throughput.
  bool jit_faststart = false;
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

// `$HOME/.cache/<name>`, falling back to `$TMPDIR` then `/tmp` when HOME is
// unset or empty (sandboxes that can't write under $HOME). Single source for
// both on-disk cache roots so their fallback behavior can't drift.
static std::filesystem::path home_cache_root(const char* name) {
  const char* home = std::getenv("HOME");
  if (!home || !*home) {
    const char* tmp = std::getenv("TMPDIR");
    home = (tmp && *tmp) ? tmp : "/tmp";
  }
  return std::filesystem::path(home) / ".cache" / name;
}

static std::filesystem::path culebra_cache_dir() {
  // Explicit override wins — useful for CI, sandboxes that can't
  // write under $HOME, or hermetic test runs that want a clean
  // cache per invocation.
  if (const char* c = std::getenv("CULEBRA_CACHE"); c && *c) return c;
  return home_cache_root("culebra") / asset_fingerprint();
}

// Bound a content-addressed cache directory so it never grows without limit.
// Both our caches key a subdirectory on a fingerprint that changes whenever
// its inputs do — the embedded runtime archives for `culebra build`, the
// (sources, link, lto) tuple for `culebra wrap` — so each new input leaves a
// stale subdirectory behind that nothing ever revisits. Users never see these
// dirs, so they can silently grow to gigabytes. Given the live subdirectory
// `current`, keep the most-recently-used few siblings (by mtime) and delete
// the rest; `current` is always retained even if it sorts old. Everything
// removed regenerates on demand, so over-pruning only costs one rebuild.
static void prune_stale_cache_dirs(const std::filesystem::path& current) {
  constexpr std::size_t keep = 4;  // live dir + a few recent ones
  std::error_code ec;
  auto root = current.parent_path();
  std::vector<std::pair<std::filesystem::file_time_type,
                        std::filesystem::path>> dirs;
  for (std::filesystem::directory_iterator it(root, ec), end; it != end;
       it.increment(ec)) {
    if (ec) return;
    if (!it->is_directory(ec)) continue;
    auto t = std::filesystem::last_write_time(it->path(), ec);
    if (ec) continue;
    dirs.emplace_back(t, it->path());
  }
  if (dirs.size() <= keep) return;
  std::sort(dirs.begin(), dirs.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  for (std::size_t i = keep; i < dirs.size(); ++i) {
    if (dirs[i].second == current) continue;  // never delete the live one
    std::filesystem::remove_all(dirs[i].second, ec);
  }
}

// Materialize one embedded runtime archive (by file name) to the on-disk
// cache and return its path. The linker needs a real file, so the driver
// writes the archive once per (culebra binary × machine) and reuses it on
// subsequent invocations.
static std::filesystem::path materialize_archive(
    const std::string& name, std::string& err) {
  auto dir = culebra_cache_dir();
  // Garbage-collect old fingerprints once, before the first archive lands.
  static std::once_flag pruned;
  std::call_once(pruned, prune_stale_cache_dirs, dir);
  auto cache = dir / name;
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
  bool keep_symbols = false;  // skip link-time symbol strip (for debugging)
  int opt_level = 2;
};
#endif  // CULEBRA_JIT_ENABLED — end of build/wrap-only helpers

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
        "  --jit-faststart    Like --jit but tuned for fast startup, not peak\n"
        "                     throughput: the FastISel backend ~halves JIT warmup\n"
        "                     (compile time) for a small steady-state cost (~7% on\n"
        "                     pure-script hot loops, ~0% when hot work is in the\n"
        "                     C++/BLAS runtime). Output matches --jit/interp.\n"
        "  --emit-llvm        Print the generated LLVM IR (with --jit)\n"
        "  -O<level>          JIT optimization level 0-3 (default 2)\n"
#endif
        "  --                 Stop parsing options; the next argument is the\n"
        "                     script even if it begins with '-'\n"
        "  -h, --help         Show this help and exit\n"
        "\n"
        "Commands:\n"
#ifdef CULEBRA_JIT_ENABLED
        "  build <in.cul> -o <out>   Compile ahead-of-time into a standalone\n"
        "                            executable (`culebra build --help`)\n"
#endif
        "  test [paths...]           Run tests / doctests (--filter, --doc,\n"
        "                            --reporter, --bail, --list)\n"
        "  lint <file.cul>...        Report static problems (errors + warnings\n"
        "                            like unused variables) without running\n"
        "  fmt [paths...]            Reformat source to canonical style\n"
        "                            (-i in-place, -l list, --check; `culebra fmt --help`)\n"
        "\n"
        "Examples:\n"
        "  culebra hello.cul              Run a script (interpreter)\n"
#ifdef CULEBRA_JIT_ENABLED
        "  culebra --jit hello.cul        Run the same script through the JIT\n"
#endif
        "  culebra                        Start the REPL\n"
        "  culebra app.cul a b c          Run with Sys.argv == [\"a\",\"b\",\"c\"]\n"
#ifdef CULEBRA_JIT_ENABLED
        "  culebra build app.cul -o app   Build a standalone binary\n"
#endif
        ;
}

#ifdef CULEBRA_JIT_ENABLED
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
        "  --keep-symbols     Keep local symbols in the output (default:\n"
        "                     stripped, ~30% smaller). Use for debugging.\n"
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
    } else if (arg == "--keep-symbols") {
      opts.keep_symbols = true;
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
  // Guard against clobbering a source file with the build artifact — the
  // classic `culebra build foo.cul -o foo.cul` slip (e.g. a tab-completion
  // that fills -o with the script name). '.cul' is the source extension and
  // never names an executable, so a '.cul' output is always a mistake; and an
  // output that resolves to the input would overwrite the script outright.
  std::string out_ext = std::filesystem::path(opts.output).extension().string();
  for (char& c : out_ext) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';  // case-insensitive (macOS FS)
  }
  if (out_ext == ".cul") {
    err = "-o: refusing to write build output to a '.cul' path ('" +
          opts.output +
          "') — '.cul' is the source extension; this would overwrite a "
          "script. Use a different output name (e.g. -o " +
          std::filesystem::path(opts.input).stem().string() + ").";
    return false;
  }
  {
    std::error_code ec;
    if (std::filesystem::exists(opts.output, ec) &&
        std::filesystem::equivalent(opts.input, opts.output, ec) && !ec) {
      err = "-o: output path is the same file as the input ('" + opts.input +
            "') — refusing to overwrite the source.";
      return false;
    }
  }
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
  // SQLite reachability force-loads libculebra_rt_sqlite.a (the strong sqlite3
  // wrappers + the bundled amalgamation object) and appends the SQLite link
  // deps, mirroring the tensor/http/compress axes.
  auto any_uses_sqlite = [&]() {
    for (const auto& m : modules) {
      if (culebra::aot_uses_sqlite(*m.ast)) return true;
    }
    return false;
  };
  // Wrap reachability force-loads the wrap archive and appends the wrapped
  // library's link flags only when the program names one of the namespaces
  // the embedded `culebra wrap` declarations registered — same usage axis as
  // tensor/http, so a binary built by a wrap-extended driver links none of the
  // wrapped library unless the script actually uses it (mirrors Http/OpenSSL).
  // The namespace set is the driver's own registry (empty for a stock binary).
  std::vector<std::string> wrap_namespaces;
  for (const auto& wc : culebra::wrapped_classes()) {
    if (std::find(wrap_namespaces.begin(), wrap_namespaces.end(), wc.ns) ==
        wrap_namespaces.end())
      wrap_namespaces.push_back(wc.ns);
  }
  auto any_uses_wrap = [&]() {
    if (wrap_namespaces.empty()) return false;
    for (const auto& m : modules) {
      if (culebra::aot_uses_any_name(*m.ast, wrap_namespaces)) return true;
    }
    return false;
  };
#ifdef CULEBRA_ENABLE_GRAPHICS
  // Graphics reachability force-loads libculebra_rt_graphics.a (the wrap
  // registration whose registrar pulls in raylib) and appends the raylib/SDL
  // link deps, mirroring the tensor/http/compress/sqlite axes. Compiled only
  // into a graphics-enabled driver: a stock build never embeds that archive, so
  // it must not try to force-load it (doing so aborts on a missing archive).
  auto any_uses_graphics = [&]() {
    for (const auto& m : modules) {
      if (culebra::aot_uses_graphics(*m.ast)) return true;
    }
    return false;
  };
#endif

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
  bool uses_sqlite = cross ? false : any_uses_sqlite();
#ifdef CULEBRA_ENABLE_GRAPHICS
  bool uses_graphics = cross ? false : any_uses_graphics();
#else
  bool uses_graphics = false;  // graphics archive isn't embedded in a stock build
#endif
  bool uses_wrap = cross ? false : any_uses_wrap();

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

  // Discard local symbols at link time (~30% smaller binary: the embedded
  // runtime archive carries thousands of local symbols — GCC_except_table*,
  // string/template instantiations — useless in a distributed executable).
  // `-Wl,-x` ("discard all local symbols") is understood by ld64, GNU ld and
  // lld alike, and keeps the global/dynamic symbols the loader needs, so the
  // binary still runs. --keep-symbols opts out for debugging.
  const char* strip_syms = opts.keep_symbols ? "" : "-Wl,-x";

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
  std::string sqlite_lib =
      (uses_sqlite && host_build)
          ? force_load_feature("libculebra_rt_sqlite.a")
          : "";
  std::string graphics_lib =
      (uses_graphics && host_build)
          ? force_load_feature("libculebra_rt_graphics.a")
          : "";
  // Wrap declarations (out-of-tree bindings, `culebra wrap`): static
  // registrars nothing references by name, so the archive member must be
  // force-loaded for the binding to register — but only when the program
  // actually names a wrapped namespace, so a script that uses none links
  // none of the wrapped library (the Http/OpenSSL gating, applied to wrap).
  // materialize_archive returns empty when the asset doesn't exist (a stock
  // binary) — skip.
  std::string wrap_lib;
  if (host_build && uses_wrap) {
    std::string err;
    auto path = materialize_archive("libculebra_rt_wrap.a", err);
    if (!path.empty()) {
      auto q = std::format("'{}'", path.string());
      wrap_lib = target_is_macho
          ? std::format("-Wl,-force_load,{}", q)
          : std::format("-Wl,--whole-archive {} -Wl,--no-whole-archive", q);
    }
  }
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
  // SQLite's own link deps (pthread/dl/m on Linux; none on macOS). The strong
  // sqlite3 wrappers + amalgamation are force-loaded via sqlite_lib above; this
  // resolves the amalgamation's platform deps. "" when SQLite is unused.
  std::string sqlite_link = uses_sqlite ? CULEBRA_SQLITE_LINK : "";
  // raylib + SDL2 statics' platform deps (GUI/audio frameworks). Appended only
  // when the program references Graphics; the wrap registrar force-loaded via
  // graphics_lib is what references the symbols these flags resolve. "" when
  // Graphics is unused or built out.
  std::string graphics_link = uses_graphics ? CULEBRA_GRAPHICS_LINK : "";
  // The wrapped library's own link flags (`culebra wrap --link`, baked at
  // wrap time) ride the same usage axis as wrap_lib above: appended only when
  // the program names a wrapped namespace, so an unused wrapped library adds
  // nothing to the binary. "" for a stock driver.
  std::string wrap_link_flags = uses_wrap ? CULEBRA_WRAP_LINK_FLAGS : "";
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

  std::string cmd = std::format(
      "{}{} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} -o {}", cc, extra,
      shq(obj), shq(lib), tensor_lib, http_lib, compress_lib, sqlite_lib,
      graphics_lib, wrap_lib, dead_strip, strip_syms, no_pie, libcxx, blas, ssl, zlib,
      sqlite_link, graphics_link, wrap_link_flags, shq(opts.output));

  if (verbose) std::println(stderr, "culebra build: link: {}", cmd);
  int link_rc = std::system(cmd.c_str());
  if (link_rc != 0) {
    std::println(stderr, "culebra build: link failed (rc={})", link_rc);
    return 1;
  }

  if (!verbose) std::filesystem::remove(obj);
  return 0;
}

// `culebra wrap decl.cpp [decl2.cpp ...] [-o out] [--link "<flags>"] [--lto]`
// — build an EXTENDED culebra binary with the wrap.h declarations compiled
// in (design §10.1 P3 / §14.3 Phase 4). The "codegen" already happened in
// the C++ templates; this subcommand is pure build orchestration: it
// rebuilds the culebra source tree with CULEBRA_WRAP_SOURCES into a cache
// dir and copies out the product. Requires a source checkout — the path
// baked at build time (CULEBRA_SOURCE_DIR), overridable with $CULEBRA_HOME.
// ccache (if configured via the usual env) makes the rebuild incremental:
// effectively compile-the-declaration + relink.
struct WrapOptions {
  vector<string> sources;
  string output = "culebra-wrapped";
  string link_flags;
  bool lto = false;
};

void print_wrap_usage(ostream& os) {
  os << "Usage: culebra wrap <decl.cpp> [decl2.cpp ...] [options]\n"
        "  -o <path>        output binary (default: ./culebra-wrapped)\n"
        "  --link <flags>   extra link flags for the wrapped library\n"
        "  --lto            build the extended binary with LTO (slower)\n"
        "  CULEBRA_HOME     source checkout to build against (default:\n"
        "                   the path this binary was built from)\n";
}

int run_wrap(int argc, const char** argv) {
  WrapOptions opts;
  for (int i = 2; i < argc; i++) {
    string a = argv[i];
    if (a == "-o" && i + 1 < argc) {
      opts.output = argv[++i];
    } else if (a == "--link" && i + 1 < argc) {
      opts.link_flags = argv[++i];
    } else if (a == "--lto") {
      opts.lto = true;
    } else if (a == "-h" || a == "--help") {
      print_wrap_usage(cout);
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      std::println(stderr, "culebra wrap: unknown option '{}'", a);
      print_wrap_usage(cerr);
      return 1;
    } else {
      std::error_code ec;
      auto abs = std::filesystem::absolute(a, ec);
      if (ec || !std::filesystem::exists(abs)) {
        std::println(stderr, "culebra wrap: can't open '{}'", a);
        return 1;
      }
      if (abs.string().find('\'') != string::npos) {
        std::println(stderr,
            "culebra wrap: path must not contain a single quote: '{}'", a);
        return 1;
      }
      opts.sources.push_back(abs.string());
    }
  }
  if (opts.sources.empty()) {
    print_wrap_usage(cerr);
    return 1;
  }
  if (opts.link_flags.find('\'') != string::npos) {
    std::println(stderr, "culebra wrap: --link must not contain a single quote");
    return 1;
  }

  // The source tree to rebuild. $CULEBRA_HOME wins; the baked path is the
  // checkout this binary came from (a dev install).
  string src_dir;
  if (const char* home = std::getenv("CULEBRA_HOME"); home && *home) {
    src_dir = home;
  } else {
#ifdef CULEBRA_SOURCE_DIR
    src_dir = CULEBRA_SOURCE_DIR;
#endif
  }
  if (src_dir.empty() ||
      !std::filesystem::exists(
          std::filesystem::path(src_dir) / "CMakeLists.txt")) {
    std::println(stderr,
        "culebra wrap: no culebra source tree at '{}' — set CULEBRA_HOME "
        "to a checkout (https://github.com/yhirose/culebra)", src_dir);
    return 1;
  }

  // One cache dir per (sources, link, lto) configuration so switching
  // declarations doesn't thrash a shared build tree.
  string fingerprint;
  for (const auto& s : opts.sources) fingerprint += s + ";";
  fingerprint += opts.link_flags + (opts.lto ? "|lto" : "");
  auto cache_root = home_cache_root("culebra-wrap");
  auto build_dir =
      cache_root / std::format("{:016x}", std::hash<string>{}(fingerprint));
  // Drop old build trees before adding another — each is a full CMake build
  // dir, so the wrap cache bloats even faster than the runtime-archive one.
  prune_stale_cache_dirs(build_dir);
  std::error_code ec;
  std::filesystem::create_directories(build_dir, ec);

  string wrap_sources;
  for (const auto& s : opts.sources) {
    if (!wrap_sources.empty()) wrap_sources += ";";
    wrap_sources += s;
  }

  auto shq = [](std::string_view v) { return std::format("'{}'", v); };
  bool verbose = std::getenv("CULEBRA_VERBOSE") != nullptr;
  auto configure = std::format(
      "cmake -S {} -B {} -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=ON "
      "-DCULEBRA_LTO={} -DCULEBRA_WRAP_SOURCES={} -DCULEBRA_WRAP_LINK={}{}",
      shq(src_dir), shq(build_dir.string()), opts.lto ? "ON" : "OFF",
      shq(wrap_sources), shq(opts.link_flags),
      verbose ? "" : " > /dev/null");
  if (verbose) std::println(stderr, "culebra wrap: configure: {}", configure);
  if (std::system(configure.c_str()) != 0) {
    std::println(stderr, "culebra wrap: cmake configure failed");
    return 1;
  }
  auto build = std::format("cmake --build {} --target culebra -j{}{}",
                           shq(build_dir.string()),
                           std::thread::hardware_concurrency(),
                           verbose ? "" : " > /dev/null");
  if (verbose) std::println(stderr, "culebra wrap: build: {}", build);
  if (std::system(build.c_str()) != 0) {
    std::println(stderr, "culebra wrap: build failed (re-run with "
                         "CULEBRA_VERBOSE=1 for the full log)");
    return 1;
  }

  auto product = build_dir / "culebra";
  std::filesystem::copy_file(product, opts.output,
      std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::println(stderr, "culebra wrap: can't write '{}': {}", opts.output,
                 ec.message());
    return 1;
  }
  std::filesystem::permissions(opts.output,
      std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
          std::filesystem::perms::others_exec,
      std::filesystem::perm_options::add, ec);
  std::println(stderr, "culebra wrap: wrote {}", opts.output);
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
      if (arg == "--jit-faststart") {
        options.jit = true;
        options.jit_faststart = true;
        continue;
      }
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
                                 options.opt_level, options.jit_faststart);
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

// `culebra lint <file.cul>...` — parse each file and report static
// diagnostics (the load-stage errors plus advisory warnings like unused
// locals) without evaluating. Each file is linted on its own (imports bind
// their namespace name, so single-file analysis is sound). Exit code: 0 when
// clean, 1 when only warnings were found, 2 when any error (or a parse /
// read failure) occurred — so CI can gate on it.
int run_lint(int argc, const char** argv) {
  vector<string> files;
  for (int i = 2; i < argc; i++) {
    string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::println("Usage: culebra lint <file.cul>...");
      return 0;
    }
    files.push_back(arg);
  }
  if (files.empty()) {
    std::println(stderr, "culebra lint: no input files");
    return 2;
  }

  int errors = 0, warnings = 0;
  bool had_failure = false;
  for (const auto& path : files) {
    vector<char> buff;
    if (!read_file(path.c_str(), buff)) {
      std::println(stderr, "culebra lint: can't open '{}'", path);
      had_failure = true;
      continue;
    }
    vector<string> parse_msgs;
    auto ast = culebra::parse(path, buff.data(), buff.size(), parse_msgs);
    if (!ast) {
      for (const auto& m : parse_msgs) std::print(stderr, "{}", m);
      had_failure = true;
      continue;
    }
    for (const auto& d : culebra::lint::collect_module(*ast)) {
      const char* sev =
          d.severity == culebra::lint::Severity::Error ? "error" : "warning";
      std::println("{}:{}:{}: {}: {}", path, d.line, d.col, sev, d.message);
      if (d.severity == culebra::lint::Severity::Error)
        errors++;
      else
        warnings++;
    }
  }

  if (errors > 0 || had_failure) return 2;
  return warnings > 0 ? 1 : 0;
}

// Expand any directory argument into the `.cul` files it contains (recursively,
// sorted for stable output); plain file arguments pass through unchanged. A
// directory that can't be read is reported via `ok=false`.
static vector<string> fmt_expand_paths(const vector<string>& args, bool& ok) {
  namespace fs = std::filesystem;
  vector<string> out;
  for (const auto& a : args) {
    std::error_code ec;
    if (!fs::is_directory(a, ec)) { out.push_back(a); continue; }
    vector<string> found;
    for (fs::recursive_directory_iterator it(a, ec), end; it != end; it.increment(ec)) {
      if (ec) { std::println(stderr, "culebra fmt: can't read '{}'", a); ok = false; break; }
      if (it->is_regular_file(ec) && it->path().extension() == ".cul")
        found.push_back(it->path().string());
    }
    std::sort(found.begin(), found.end());
    out.insert(out.end(), found.begin(), found.end());
  }
  return out;
}

// `culebra fmt [paths...]` — reformat Culebra source to the canonical style.
// Default: write the formatted result to stdout. `-i`/`--in-place` rewrites
// files in place. `-l`/`--list` prints the names of files that would change
// (and exits 1). `--check` is like `-l` but prints nothing. A directory
// argument is expanded to the `.cul` files under it (recursively). With no
// paths (or `-`) it formats stdin to stdout (for editor format-on-save). Exit
// code: 0 clean, 1 when `-l`/`--check` found changes, 2 on parse / read /
// safety failure.
int run_fmt(int argc, const char** argv) {
  bool in_place = false, list = false, check = false;
  vector<string> files;
  for (int i = 2; i < argc; i++) {
    string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::println("Usage: culebra fmt [-i|--in-place] [-l|--list] [--check] "
                   "[paths...]\n"
                   "  (no flags)     write formatted source to stdout\n"
                   "  -i, --in-place  rewrite each file in place\n"
                   "  -l, --list      list files that would change (exit 1)\n"
                   "  --check         exit 1 if any file would change (no output)\n"
                   "  paths           files, or directories scanned for *.cul\n"
                   "  -               read from stdin, write to stdout");
      return 0;
    }
    if (arg == "-i" || arg == "--in-place") { in_place = true; continue; }
    if (arg == "-l" || arg == "--list") { list = true; continue; }
    if (arg == "--check") { check = true; continue; }
    files.push_back(arg);
  }

  bool expand_ok = true;
  // `-` (stdin) is not a path to expand; only expand when there are real paths.
  bool has_stdin = false;
  for (auto& f : files) if (f == "-") has_stdin = true;
  if (!has_stdin) files = fmt_expand_paths(files, expand_ok);

  auto report = [](const string& path, const culebra::fmt::FormatResult& r) {
    if (r.status == culebra::fmt::FormatStatus::ParseError) {
      std::print(stderr, "{}", r.message);
    } else if (r.status == culebra::fmt::FormatStatus::Refused) {
      std::println(stderr, "{}: {}", path, r.message);
    }
  };

  auto fmt = [&](const std::string& p, std::string_view s) {
    return culebra::fmt::format_source(p, s);
  };

  // stdin -> stdout when no file arguments (or the lone `-`).
  bool use_stdin = files.empty();
  for (auto& f : files) if (f == "-") use_stdin = true;
  if (use_stdin) {
    std::string src((std::istreambuf_iterator<char>(cin)),
                    std::istreambuf_iterator<char>());
    auto r = fmt("<stdin>", src);
    report("<stdin>", r);
    if (r.status == culebra::fmt::FormatStatus::ParseError ||
        r.status == culebra::fmt::FormatStatus::Refused)
      return 2;
    bool changed = r.status == culebra::fmt::FormatStatus::Ok;
    if (check) return changed ? 1 : 0;
    if (list) { if (changed) std::println("<stdin>"); return changed ? 1 : 0; }
    std::print("{}", r.output);
    return 0;
  }

  int rc = expand_ok ? 0 : 2;
  bool any_changed = false;
  for (const auto& path : files) {
    vector<char> buff;
    if (!read_file(path.c_str(), buff)) {
      std::println(stderr, "culebra fmt: can't open '{}'", path);
      rc = 2;
      continue;
    }
    std::string src(buff.begin(), buff.end());
    auto r = fmt(path, src);
    report(path, r);
    if (r.status == culebra::fmt::FormatStatus::ParseError ||
        r.status == culebra::fmt::FormatStatus::Refused) {
      rc = 2;
      continue;
    }
    bool changed = r.status == culebra::fmt::FormatStatus::Ok;
    if (changed) any_changed = true;
    if (check) continue;
    if (list) { if (changed) std::println("{}", path); continue; }
    if (in_place) {
      if (changed) {
        ofstream ofs(path, ios::out | ios::binary | ios::trunc);
        ofs << r.output;
      }
    } else {
      std::print("{}", r.output);
    }
  }
  if ((check || list) && any_changed && rc == 0) return 1;
  return rc;
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
  if (argc >= 2 && string(argv[1]) == "lint") {
    return run_lint(argc, argv);
  }
  if (argc >= 2 && string(argv[1]) == "fmt") {
    return run_fmt(argc, argv);
  }
  if (argc >= 2 && string(argv[1]) == "dap") {
    // Debug Adapter Protocol server over stdio (interp-backed). The program to
    // debug + its args arrive in the `launch` request, not on the command line.
    culebra::DapServer server(/*in=*/0, /*out=*/1, /*argv=*/{});
    return server.run();
  }
#ifdef CULEBRA_JIT_ENABLED
  if (argc >= 2 && string(argv[1]) == "wrap") {
    return run_wrap(argc, argv);
  }
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
