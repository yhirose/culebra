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
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>

using namespace std;

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
  return true;
}

int run_build(const BuildOptions& opts) {
  vector<char> buff;
  if (!read_file(opts.input.c_str(), buff)) {
    std::println(stderr, "culebra build: can't open '{}'", opts.input);
    return 1;
  }
  auto entry_src = culebra::prepend_stdlib_preamble(
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

  std::string extra;
  if (cross) extra += std::format(" --target={}", opts.target);
  if (!opts.sysroot.empty())
    extra += std::format(" --sysroot={}", opts.sysroot);

  std::string cmd = std::format("{}{} {} {} {} {} {} {} -o {}", cc, extra,
                                obj, lib, dead_strip, no_pie, libcxx, blas,
                                opts.output);

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
#ifdef CULEBRA_JIT_ENABLED
  bool needs_preamble = !options.jit;
#else
  bool needs_preamble = true;
#endif
  if (needs_preamble) culebra::load_stdlib_modules(env);

  for (auto path : options.script_path_list) {
    vector<char> buff;
    if (!read_file(path.c_str(), buff)) {
      std::println(stderr, "can't open '{}'.", path);
      return false;
    }

    vector<string> msgs;
    std::string_view user_src(buff.data(), buff.size());

    // Walk the dependency graph via ModuleLoader. The same vector
    // feeds both backends — JIT bundles every module into one IR,
    // interp evaluates them sequentially.
    culebra::ModuleLoader loader;
    std::vector<culebra::LoadedModule> modules;
    // Entry source goes in with the stdlib preamble already prepended
    // so the JIT path (which doesn't pre-load Time / Args via env)
    // sees those classes as ordinary top-level declarations.
    auto entry_src = culebra::prepend_stdlib_preamble(user_src);
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
    if (culebra::interpret_modules(modules, env, val, msgs, dbg)) continue;
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
  for (int i = 2; i < argc; i++) {
    std::string arg(argv[i]);
    if (arg.starts_with("--filter=")) {
      filter = arg.substr(9);
    } else if (arg == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    } else if (arg.starts_with("--")) {
      std::println(stderr, "culebra test: unknown option '{}'", arg);
      return 2;
    } else {
      roots.push_back(arg);
    }
  }
  auto env = culebra::environment({});
  culebra::install_test_ambient(*env);

  auto files = culebra::discover_test_files(roots);
  if (files.empty()) {
    std::println(stderr,
        "culebra test: no test files found (looking for test_*.cul)");
    return 1;
  }

  auto summary = culebra::run_tests(files, filter, env);
  if (summary.errored_files > 0) {
    std::println("{} passed, {} failed, {} file(s) errored",
                  summary.passed, summary.failed, summary.errored_files);
  } else {
    std::println("{} passed, {} failed", summary.passed, summary.failed);
  }
  return (summary.failed == 0 && summary.errored_files == 0) ? 0 : 1;
}

int main(int argc, const char** argv) {
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

#ifdef CULEBRA_JIT_ENABLED
  culebra::install_jit_stdlib();
#endif

  try {
    auto env = culebra::environment(options.script_argv);
    install_cli_aliases(*env);

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
