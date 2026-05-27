#include <culebra.h>
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

// Materialize one of the embedded runtime archives to the on-disk
// cache and return its path. The linker needs a real file, so the
// driver writes the archive once per (culebra binary × machine) and
// reuses it on subsequent invocations.
static std::filesystem::path materialize_rt_archive(
    bool uses_tensor, std::string& err) {
  const char* name =
      uses_tensor ? "libculebra_rt.a" : "libculebra_rt_no_tensor.a";
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

  std::ofstream out(cache, std::ios::binary | std::ios::trunc);
  if (!out) {
    err = std::format("can't write '{}'", cache.string());
    return {};
  }
  out.write(reinterpret_cast<const char*>(data->data()),
            static_cast<std::streamsize>(data->size()));
  if (!out) {
    err = std::format("write failed for '{}'", cache.string());
    return {};
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
  auto entry_src = culebra::prepend_stdlib_preamble_selective(
      std::string_view(buff.data(), buff.size()));
  vector<string> msgs;
  culebra::ModuleLoader loader;
  std::vector<culebra::LoadedModule> modules;
  try {
    modules = loader.load_program(opts.input, entry_src, msgs);
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

  // Tensor reachability drives the archive choice. The no-tensor
  // archive's stubbed tensor entry points break the static
  // reachability chain to cblas so Accelerate / BLAS drops out too.
  std::string lib;
  if (!opts.rt_lib.empty()) {
    lib = opts.rt_lib;
  } else if (cross) {
    std::println(stderr,
        "culebra build: --target=<triple> requires --rt-lib=<path>");
    return 1;
  } else {
    std::string err;
    auto path = materialize_rt_archive(uses_tensor, err);
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
  std::string blas = uses_tensor ? CULEBRA_BLAS_LINK : "";
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

  std::string cmd = std::format("{}{} {} {} {} {} {} {} -o {}", cc, extra,
                                shq(obj), shq(lib), dead_strip, no_pie,
                                libcxx, blas, shq(opts.output));

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

  int argi = 1;
  bool past_separator = false;
  while (argi < argc) {
    string arg = argv[argi++];
    if (past_separator) {
      options.script_argv.push_back(std::move(arg));
      continue;
    }
    if (arg == "--") {
      past_separator = true;
    } else if (arg == "--shell") {
      options.shell = true;
    } else if (arg == "--ast") {
      options.print_ast = true;
    } else if (arg == "--debug") {
      options.debug = true;
#ifdef CULEBRA_JIT_ENABLED
    } else if (arg == "--jit") {
      options.jit = true;
    } else if (arg == "--emit-llvm") {
      options.emit_llvm = true;
    } else if (arg.starts_with("-O")) {
      options.opt_level = std::stoi(arg.substr(2));
#endif
    } else {
      options.script_path_list.push_back(arg);
    }
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
    culebra::ModuleLoader loader;
    std::vector<culebra::LoadedModule> modules;
    std::string jit_src;
    std::string_view entry_src = user_src;
#ifdef CULEBRA_JIT_ENABLED
    if (options.jit) {
      jit_src = culebra::prepend_stdlib_preamble_selective(user_src);
      entry_src = jit_src;
    }
#endif
    try {
      modules = loader.load_program(path, entry_src, msgs);
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
    } else if (arg.starts_with("--")) {
      std::println(stderr, "culebra test: unknown option '{}'", arg);
      return 2;
    } else {
      roots.push_back(arg);
    }
  }
  auto env = culebra::environment({});
  install_cli_aliases(*env);
  culebra::install_test_ambient(*env);

  auto files = culebra::discover_test_files(roots);
  if (files.empty()) {
    std::println(stderr,
        "culebra test: no test files found (looking for test_*.cul)");
    return 1;
  }

  auto summary = culebra::run_tests(
      files, filter, env, reporter, bail_after, list_only);
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
      culebra::repl(env, options.print_ast, options.jit);
#else
      culebra::repl(env, options.print_ast);
#endif
    }
  } catch (const culebra::CulebraError& e) {
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
