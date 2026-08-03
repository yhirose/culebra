#include <compress.h>  // gunzip() — the embedded runtime archives are stored
                       // compressed (transitive too, but this file names it)
#include <culebra.h>
#include <dap.h>
#include <docs_cmd.h>
#include <doctest_runner.h>
#include <formatter.h>
#include <init_cmd.h>
#include <source_dir.h>
#include <stdlib_interp.h>
#include <test_runner.h>
#include <vfs.h>  // main_script_dir() — set here for Embed's dev disk fallback
                  // (pulled transitively only on the JIT/AOT path otherwise)
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
#include <optional>
#include <print>
#include <set>
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
// phase mark to stderr immediately.
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

// Read a whole file as bytes. nullopt when it can't be read — every caller
// prints its own message, since the phrasing is per-subcommand.
optional<string> read_file(const char* path) {
  std::error_code ec;
  // A directory opens like a file and then fails every read — libstdc++ throws
  // out of underflow rather than setting badbit. Reject it up front so callers
  // report it in their own words like any other unreadable path.
  if (std::filesystem::is_directory(path, ec) && !ec) return nullopt;

  ifstream ifs(path, ios::in | ios::binary);
  if (ifs.fail()) return nullopt;

  string buff;
  if (std::filesystem::is_regular_file(path, ec) && !ec) {
    // Sizing the buffer up front, the fast path for the file every caller
    // actually passes.
    auto size = static_cast<streamoff>(ifs.seekg(0, ios::end).tellg());
    if (ifs.good() && size > 0) {
      buff.resize(static_cast<size_t>(size));
      ifs.seekg(0, ios::beg).read(buff.data(), static_cast<streamsize>(size));
      if (!ifs) return nullopt;   // shrank or errored between sizing and reading
    }
    return buff;
  }
  // A pipe or process substitution (`culebra <(gen.sh)`) can't seek, so it has
  // no size to size a buffer with; reporting the empty string there ran an
  // empty program in place of the caller's script. Stream it instead.
  buff.assign(istreambuf_iterator<char>(ifs), istreambuf_iterator<char>());
  if (ifs.bad()) return nullopt;
  return buff;
}

// Report a CulebraError on stderr in the CLI's canonical shape —
// `Kind: message at LINE:COL.`, position omitted when unknown. Shared so the
// `build`, script-run and top-level handlers can't drift apart.
void print_culebra_error(const culebra::CulebraError& e) {
  cerr << e.kind << ": " << e.what();
  if (e.line > 0 || e.col > 0) cerr << " at " << e.line << ":" << e.col << ".";
  cerr << endl;
}

// Load the entry script plus its import graph. `splice_preamble` inlines the
// stdlib preamble into the entry module's AST, which the JIT/AOT path needs
// because it doesn't honour the env's lazy bindings; it happens *after* parse so
// user line numbers stay natural and error locations match interp (see
// splice_stdlib_preamble). Returns false having already reported the failure —
// `build` and a script run differ only in what they return, so sharing this
// keeps their diagnostics identical.
bool load_entry_program(const string& path, std::string_view src,
                        bool splice_preamble,
                        std::vector<culebra::LoadedModule>& modules) {
  vector<string> msgs;
  culebra::ModuleLoader loader;
  try {
    modules = loader.load_program(path, src, msgs);
    if (splice_preamble) culebra::splice_stdlib_preamble(modules);
  } catch (const culebra::CulebraError& e) {
    print_culebra_error(e);
    return false;
  }
  if (modules.empty()) {
    for (const auto& msg : msgs) cerr << msg << endl;
    return false;
  }
  return true;
}

struct Options {
  bool print_ast = false;
  bool shell = false;
  bool debug = false;
  bool help = false;
  bool version = false;
#ifdef CULEBRA_JIT_ENABLED
  bool jit = false;
  // Unoptimized IR pipeline + unoptimized backend: collapses JIT warmup
  // (startup/compile time) by ~40x at a small steady-state cost (~12% on
  // pure-script hot loops; ~0% when the hot work lives in the C++/BLAS
  // runtime, e.g. Tensor). Output is interp-symmetric. Opt-in: the default
  // --jit (O2) keeps the best steady-state throughput. The two levels move
  // together — see JIT::apply_fast_codegen for why an optimized IR
  // pipeline over an unoptimized backend is not a configuration we allow.
  bool jit_faststart = false;
  bool emit_llvm = false;
  int opt_level = 2;
  bool opt_level_explicit = false;
#endif
  // The entry script, if one was given. At most one: the first non-flag
  // argument is the script and everything after it is its argv.
  optional<string> script_path;
  vector<string> script_argv;
  string error;  // non-empty: a malformed flag; main reports it and exits
};

#ifdef CULEBRA_JIT_ENABLED
// Quote a path so $TMPDIR / paths with spaces survive verbatim through
// std::system (validate_build_path rejects the quote char, so it can't escape).
// POSIX `system()` runs the command via /bin/sh (single-quote quoting); mingw
// `system()` runs it via cmd.exe, where single quotes are literal and double
// quotes are the quoting form. Pick per-platform so paths with spaces survive on
// both. (mingw g++ itself accepts forward-slash paths.) Shared by the
// embedded-assets compile, the `build` link and the `wrap` cmake invocations.
static std::string shq(std::string_view s) {
#ifdef _WIN32
  return std::format("\"{}\"", s);
#else
  return std::format("'{}'", s);
#endif
}

// Where a std::system() command's chatter goes when it isn't wanted. Same split
// as shq: cmd.exe has no /dev/null, and redirecting there makes the command
// fail on a missing directory instead of going quiet.
#ifdef _WIN32
constexpr std::string_view kNullDevice = "NUL";
#else
constexpr std::string_view kNullDevice = "/dev/null";
#endif

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

// $CULEBRA_CACHE wins over $HOME/.cache/culebra — useful for CI, sandboxes
// that can't write under $HOME, or hermetic test runs that want a clean cache
// per invocation. The archives land in a fingerprint subdirectory either way,
// so an override names a root we fill rather than a directory we overwrite.
static std::filesystem::path culebra_cache_dir() {
  const char* c = std::getenv("CULEBRA_CACHE");
  auto root = (c && *c) ? std::filesystem::path(c) : home_cache_root("culebra");
  return root / asset_fingerprint();
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
    // Only ever delete a directory shaped like the fingerprints we write (8 or
    // 16 lowercase hex digits), because the root can hold entries we don't own:
    // the build-time SDL/raylib statics cache (CMake's CULEBRA_DEPS_CACHE) is a
    // `deps` sibling by default, and $CULEBRA_CACHE can name a directory with
    // unrelated contents. Pruning `deps` alone would silently break every AOT
    // link that force-loads a windowed feature archive until the ~3.5min deps
    // build is redone.
    auto name = it->path().filename().string();
    if (name.find_first_not_of("0123456789abcdef") != std::string::npos)
      continue;
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

// Scratch files a build writes next to its output: removed when the scope that
// registered them ends, so the early returns (a failed asset compile, a failed
// link) leave no more behind than the happy path does. Removing a path that was
// never written, or that a rename already claimed, is a no-op. `keep`
// (CULEBRA_VERBOSE) leaves them for inspection — the verbose log prints each
// one, and reading the generated asset source is the point of the flag.
class ScratchFiles {
 public:
  explicit ScratchFiles(bool keep = false) : keep_(keep) {}
  ScratchFiles(const ScratchFiles&) = delete;
  ScratchFiles& operator=(const ScratchFiles&) = delete;
  ~ScratchFiles() {
    if (keep_) return;
    std::error_code ec;
    for (const auto& p : paths_) std::filesystem::remove(p, ec);
  }
  void add(std::string path) { paths_.push_back(std::move(path)); }

 private:
  std::vector<std::string> paths_;
  bool keep_;
};

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

  // The entries are embedded compressed (see CMakeLists).
  auto it = CulebraRT::FS.find(name);
  if (it == CulebraRT::FS.end()) {
    err = std::format("embedded runtime archive '{}' not found", name);
    return {};
  }
  auto packed = (*it).text();
  if (!packed) {
    err = std::format("embedded runtime archive '{}' has no data", name);
    return {};
  }
  auto archive = culebra::compress::gunzip(*packed);
  if (!archive.error.empty()) {
    err = std::format("embedded runtime archive '{}' is corrupt: {}", name,
                      archive.error);
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
  ScratchFiles scratch;
  scratch.add(tmp.string());
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      err = std::format("can't write '{}'", tmp.string());
      return {};
    }
    out.write(archive.data.data(),
              static_cast<std::streamsize>(archive.data.size()));
    if (!out) {
      err = std::format("write failed for '{}'", tmp.string());
      return {};
    }
  }
  std::filesystem::rename(tmp, cache, ec);
  if (ec) {
    // Lost a race (another process renamed first) or cross-device link —
    // if the destination is now a complete archive, use it; else report.
    if (!std::filesystem::exists(cache)) {
      err = std::format("can't finalize '{}': {}", cache.string(),
                        ec.message());
      return {};
    }
  }
  return cache;
}

// Does this driver embed the feature's archive? CMake appends each gated
// archive to _rt_embed_files under the same switch, and force-loading an
// archive that isn't embedded aborts the build — so the axis table carries the
// bit. Normalized to bools here to keep the table one row per axis.
static constexpr bool kEmbedsSqlite =
#if defined(CULEBRA_SQLITE_ENABLED)
    true;
#else
    false;
#endif
static constexpr bool kEmbedsScene =
#if defined(CULEBRA_ENABLE_SCENE)
    true;
#else
    false;
#endif
static constexpr bool kEmbedsCanvas =
#if defined(CULEBRA_CANVAS_WINDOW)
    true;
#else
    false;
#endif
static constexpr bool kEmbedsWebview =
#if defined(CULEBRA_ENABLE_WEBVIEW)
    true;
#else
    false;
#endif

// One AOT feature axis: a stdlib namespace whose use pulls an extra runtime
// archive and its link flags into the built binary. The core archive is
// axis-free — every heavy dependency sits behind a weak stub there (cblas via
// tensor_eval_node, OpenSSL/zlib via http_request, …) — so the strong bodies
// are force-loaded from these per-feature objects only when the program names
// the axis. One core archive, N feature objects; no 2^N variant matrix.
//
// A `-l`/archive append would NOT do it: the weak definition already satisfies
// the symbol, so the member is never loaded (verified). The object has to be
// force-loaded, which is what run_build's force_load_feature emits.
//
// Adding an axis is one row here plus its CMake link fragment — the scan, the
// force-load and the link append all read this table.
struct FeatureAxis {
  const char* names[3];    // namespaces that trigger it (trailing may be null)
  const char* archive;     // force-loaded on a hit
  const char* link_flags;  // appended on a hit ("" when built out)
  bool embedded;           // this driver carries `archive`
};

static constexpr FeatureAxis kFeatureAxes[] = {
    // BLAS is reachable only through the tensor_eval_node choke, so a non-Tensor
    // program references no cblas symbol and drops the link entirely.
    {{"Tensor"}, "libculebra_rt_tensor.a", CULEBRA_BLAS_LINK, true},
    // CULEBRA_SSL_LINK carries OpenSSL *and* zlib; a non-Http binary is ~4 MB
    // lighter for skipping it.
    {{"Http"}, "libculebra_rt_http.a", CULEBRA_SSL_LINK, true},
    // A duplicate -lz when Http is also used is harmless (the linker dedupes).
    // `to_png` rides this axis: the PNG encoder deflates through the same zlib
    // choke (see image.h), so a program that encodes an image needs the archive
    // even when it never names `Compress`. Both spellings are listed — the
    // public method and the `_Canvas` primitive the Canvas tests call — because
    // missing one links the weak stub and the call raises "runtime not linked".
    {{"Compress", "to_png", "sprite_to_png"},
     "libculebra_rt_compress.a", CULEBRA_ZLIB_LINK, true},
    // The archive bundles the amalgamation object too, so a force-loaded
    // culebra_rt_sqlite is self-contained; the flags are its platform deps.
    {{"SQLite"}, "libculebra_rt_sqlite.a", CULEBRA_SQLITE_LINK, kEmbedsSqlite},
    // Scene and Webview have no weak choke — they simply aren't in the core
    // archive, so a program that names neither references no raylib/WebKit
    // symbol. What gets force-loaded is the wrap registrar nothing calls by name.
    {{"Scene"}, "libculebra_rt_scene.a", CULEBRA_SCENE_LINK, kEmbedsScene},
    // Canvas does have a weak choke: the headless present/input stubs in the
    // core archive, overridden by the raylib bodies in this archive.
    {{"Canvas"}, "libculebra_rt_canvas.a", CULEBRA_CANVAS_LINK, kEmbedsCanvas},
    // `Desktop` is the facade that drives Webview, so naming it has to trigger
    // the axis even when the program never says `Webview`.
    {{"Webview", "Desktop"}, "libculebra_rt_webview.a", CULEBRA_WEBVIEW_LINK,
     kEmbedsWebview},
};
// Cross-compiling with Tensor would need a target-specific BLAS link, which we
// don't bundle — run_build rejects that pair up front by this row.
static constexpr size_t kTensorAxis = 0;

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
        "                     throughput: skipping both the IR and the backend\n"
        "                     optimizers cuts JIT warmup (compile time) by ~40x\n"
        "                     for a small steady-state cost (~12% on pure-script\n"
        "                     hot loops, ~0% when hot work is in the C++/BLAS\n"
        "                     runtime). Implies -O0. Output matches --jit/interp.\n"
        "  --emit-llvm        Print the generated LLVM IR (with --jit)\n"
        "  -O<level>          JIT optimization level 0-3 (default 2)\n"
#endif
        "  --                 Stop parsing options; the next argument is the\n"
        "                     script even if it begins with '-'\n"
        "  -h, --help         Show this help and exit\n"
        "  --version          Show the version and exit\n"
        "\n"
        "Commands:\n"
#ifdef CULEBRA_JIT_ENABLED
        "  build <in.cul> -o <out>   Compile ahead-of-time into a standalone\n"
        "                            executable (`culebra build --help`)\n"
#endif
        "  test [paths...]           Run tests / doctests (--filter, --doc,\n"
        "                            --reporter, --bail, --list)\n"
        "  lint [paths...]           Report static problems (errors + warnings\n"
        "                            like unused variables) without running\n"
        "                            (--fix removes unused imports; `culebra lint --help`)\n"
        "  fmt [paths...]            Reformat source to canonical style\n"
        "                            (-i in-place, -l list, --check; `culebra fmt --help`)\n"
        "  dap                       Speak the Debug Adapter Protocol over\n"
        "                            stdin/stdout (your editor launches this)\n"
        "  docs [topic]              Read the reference docs carried in this\n"
        "                            binary (-g searches them; `culebra docs`\n"
        "                            lists the topics)\n"
        "  init                      Set up this directory (AI agent\n"
        "                            instructions) and this machine's editors\n"
        "                            (VSCode/Vim/Neovim); safe to re-run\n"
#ifdef CULEBRA_JIT_ENABLED
        "  wrap <decl.cpp> ...       Build an extended culebra binary exposing\n"
        "                            your C++ classes (`culebra wrap --help`)\n"
#endif
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
        "                     target. Defaults to the host build.\n"
        "  CULEBRA_HOME       Source checkout to take headers from when the\n"
        "                     program uses Embed.dir (default: the path this\n"
        "                     binary was built from)\n";
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

// Path values are quoted by shq() before reaching `cc` via std::system, so the
// validator's job is to reject anything that breaks out of that quoting plus
// control chars. Other shell metacharacters are harmless inside quotes but
// rejected defensively — none appear in realistic file paths. The two shells
// part ways on two characters: `\` is Windows' path separator, so `-o
// build\out.exe` has to be accepted there, while `%` expands inside cmd.exe's
// double quotes the way `$` does inside /bin/sh's.
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
               uc == '*' || uc == '?';
#ifdef _WIN32
    bad = bad || uc == '%';
#else
    bad = bad || uc == '\\';
#endif
    if (bad) {
      err = string(flag) + ": path contains unsupported character";
      return false;
    }
  }
  return true;
}

// Parse a `-O<level>` flag. Rejects a non-numeric or out-of-range level rather
// than letting std::stoi throw: both command-line parsers run before main's try
// block, so an escaping exception would abort the process instead of printing.
static bool parse_opt_level(std::string_view arg, int& level, string& err) {
  auto digits = arg.substr(2);
  if (digits.size() != 1 || digits[0] < '0' || digits[0] > '3') {
    err = std::format("-O: expected a level 0-3, got '{}'", digits);
    return false;
  }
  level = digits[0] - '0';
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
      if (!parse_opt_level(arg, opts.opt_level, err)) return false;
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
  auto user_src = read_file(opts.input.c_str());
  if (!user_src) {
    std::println(stderr, "culebra build: can't open '{}'", opts.input);
    return 1;
  }
  // AOT always needs the preamble spliced in — it runs the JIT's lowering.
  std::vector<culebra::LoadedModule> modules;
  if (!load_entry_program(opts.input, *user_src, true, modules)) return 1;

  // Scratch object goes in the platform temp dir. temp_directory_path()
  // honours TMPDIR/TMP/TEMP and falls back to /tmp on POSIX; on Windows it
  // returns %TEMP% (a real, writable dir — the bare "/tmp" fallback used
  // before doesn't exist there, so the object write failed).
  std::error_code tmp_ec;
  auto tmpdir = std::filesystem::temp_directory_path(tmp_ec);
  if (tmp_ec) tmpdir = "/tmp";
  auto stem = std::filesystem::path(opts.output).filename().string();
  auto obj =
      (tmpdir / std::format("{}.{}.o", stem, static_cast<unsigned>(::getpid())))
          .string();

  bool verbose = std::getenv("CULEBRA_VERBOSE") != nullptr;
  if (verbose) std::println(stderr, "culebra build: object -> {}", obj);
  ScratchFiles scratch(verbose);
  scratch.add(obj);

  bool cross = !opts.target.empty();
  if (cross && opts.target == llvm::sys::getDefaultTargetTriple()) {
    cross = false;  // a no-op cross is just a host build
  }

  // Walk the whole dependency graph once per axis.
  bool used[std::size(kFeatureAxes)] = {};
  for (size_t i = 0; i < std::size(kFeatureAxes); i++) {
    std::vector<std::string_view> names;
    for (const char* n : kFeatureAxes[i].names)
      if (n) names.push_back(n);
    for (const auto& m : modules) {
      if (culebra::aot_uses_any_name(*m.ast, names)) {
        used[i] = true;
        break;
      }
    }
  }

  // Reject early: cross-compile + Tensor would need a target-specific BLAS
  // link flag, which Phase E doesn't bundle.
  if (cross && used[kTensorAxis]) {
    std::println(stderr,
        "culebra build: --target=<triple> with Tensor not yet "
        "supported. Drop Tensor references for now.");
    return 1;
  }
  // A cross build carries its feature support in --rt-lib, so none of the
  // embedded feature objects is force-loaded and no host link flag is appended.
  if (cross) {
    for (auto& u : used) u = false;
  }

  // The wrap axis rides the same usage gate but can't be a table row: its
  // namespaces come from the driver's own registry (empty for a stock binary)
  // rather than a literal, so a binary built by a wrap-extended driver links
  // none of the wrapped library unless the script names one of its namespaces.
  std::vector<std::string_view> wrap_namespaces;
  for (const auto& wc : culebra::wrapped_classes()) {
    if (std::find(wrap_namespaces.begin(), wrap_namespaces.end(), wc.ns) ==
        wrap_namespaces.end())
      wrap_namespaces.push_back(wc.ns);
  }
  bool uses_wrap = false;
  if (!cross && !wrap_namespaces.empty()) {
    for (const auto& m : modules) {
      if (culebra::aot_uses_any_name(*m.ast, wrap_namespaces)) {
        uses_wrap = true;
        break;
      }
    }
  }

  int rc = culebra::JIT::build_object(modules, obj, opts.opt_level,
                                       opts.emit_llvm, opts.target);
  if (rc != 0) return rc;

  // --- Embedded assets: bake each `Embed.dir("...")` directory into an object
  // file linked alongside the program. The object reproduces the directory as a
  // static AssetEntry table and registers it (under the dir name) at static-init
  // time; at runtime `http_server_serve_embed` finds the baked table and serves
  // from it instead of disk — so the binary needs no external asset files. Dirs
  // are resolved relative to the entry script (the same base the dev disk path
  // uses). A non-literal `Embed.dir(expr)` is skipped here and falls back to
  // live disk at runtime.
  std::string assets_obj;
  {
    std::vector<std::string> embed_dirs;
    for (const auto& m : modules)
      culebra::aot_collect_embed_dirs(*m.ast, embed_dirs);
    if (!embed_dirs.empty()) {
      namespace fs = std::filesystem;
      auto src_dir = culebra::resolved_source_dir();
      if (src_dir.empty() || !fs::exists(fs::path(src_dir) / "include")) {
        std::println(stderr,
            "culebra build: Embed.dir needs the culebra headers; no include/ "
            "at '{}' — set CULEBRA_HOME to a checkout", src_dir);
        return 1;
      }
      fs::path base = fs::path(opts.input).parent_path();
      auto cstr = [](std::string_view s) {
        std::string o;
        for (char c : s) {
          if (c == '\\' || c == '"') o += '\\';
          o += c;
        }
        return o;
      };
      std::string src = "#include <vfs.h>\nnamespace {\n";
      size_t ti = 0;
      for (const auto& dir : embed_dirs) {
        fs::path root = base / dir;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
          std::println(stderr,
              "culebra build: Embed.dir(\"{}\"): not a directory at '{}'", dir,
              root.string());
          return 1;
        }
        // Every asset has to make it into the table. A walk that stops early or
        // a file that won't open used to be skipped in silence, baking a table
        // with holes in it: the build reports success and the binary 404s the
        // missing files at runtime.
        auto fail_read = [&](const fs::path& p, std::string_view why) {
          std::println(stderr,
              "culebra build: Embed.dir(\"{}\"): can't read '{}': {}", dir,
              p.string(), why);
          return 1;
        };
        std::vector<std::pair<std::string, std::string>> files;  // rel, bytes
        // The error check has to come before the end test: a failed increment
        // (a subdirectory that won't open) leaves the iterator equal to end, so
        // testing that first would exit the walk as if it had finished.
        for (fs::recursive_directory_iterator it(root, ec), end;; it.increment(ec)) {
          if (ec) return fail_read(root, ec.message());
          if (it == end) break;
          bool regular = it->is_regular_file(ec);
          if (ec) return fail_read(it->path(), ec.message());
          if (!regular) continue;
          auto rel = fs::relative(it->path(), root, ec).generic_string();
          if (ec || rel.empty()) return fail_read(it->path(), "no path under the directory");
          std::ifstream f(it->path(), std::ios::binary);
          if (!f) return fail_read(it->path(), "open failed");
          std::string bytes((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
          if (f.bad()) return fail_read(it->path(), "read failed");
          files.emplace_back(std::move(rel), std::move(bytes));
        }
        std::sort(files.begin(), files.end());
        std::uintmax_t total = 0;
        for (size_t fi = 0; fi < files.size(); fi++) {
          src += std::format("static const unsigned char a{}_{}[]={{", ti, fi);
          for (unsigned char c : files[fi].second)
            src += std::format("{},", static_cast<int>(c));
          src += "0};\n";  // trailing 0 keeps empty-file arrays well-formed
          total += files[fi].second.size();
        }
        src += std::format("static const culebra::AssetEntry t{}[]={{", ti);
        for (size_t fi = 0; fi < files.size(); fi++)
          src += std::format("{{\"{}\",a{}_{},{}}},", cstr(files[fi].first), ti,
                             fi, files[fi].second.size());
        if (files.empty()) src += "{nullptr,nullptr,0}";  // non-empty array
        src += "};\n";
        src += std::format(
            "struct Reg{0}{{Reg{0}(){{culebra::register_asset_table(\"{1}\",t{0}"
            ",{2});}}}};static Reg{0} reg{0};\n",
            ti, cstr(dir), files.size());
        std::println(stderr,
            "culebra build: embedded {} file(s) ({} bytes) from '{}'",
            files.size(), total, dir);
        ti++;
      }
      src += "}\n";
      auto acpp = (tmpdir / std::format("{}.assets.{}.cpp", stem,
                                        static_cast<unsigned>(::getpid())))
                      .string();
      auto aobj = (tmpdir / std::format("{}.assets.{}.o", stem,
                                        static_cast<unsigned>(::getpid())))
                      .string();
      scratch.add(acpp);
      scratch.add(aobj);
      { std::ofstream(acpp) << src; }
      auto ccmd = std::format("c++ -std=c++23 -O2 -I {}/include -c {} -o {}",
                              shq(src_dir), shq(acpp), shq(aobj));
      if (verbose) std::println(stderr, "culebra build: assets: {}", ccmd);
      if (std::system(ccmd.c_str()) != 0) {
        std::println(stderr, "culebra build: failed to compile embedded assets");
        return 1;
      }
      assets_obj = shq(aobj);  // pre-quoted for the link line
    }
  }

  // The core archive is feature-axis-free (see kFeatureAxes), so the same
  // archive serves every program.
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

  // Link driver: `cc` on POSIX; on Windows the runtime archive is C++
  // (libstdc++/exceptions), so drive the link with mingw `g++` to pull the
  // right default libs. The produced .exe is statically linked (below) so it
  // runs on a bare Windows like the interpreter build.
#ifdef _WIN32
  const char* cc = "g++";
#else
  const char* cc = "cc";
#endif

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

  // Emit the force-load fragment for one feature archive (see kFeatureAxes for
  // why a plain `-l`/archive append won't pull it in). `optional` reports a
  // missing archive as "no fragment" rather than an error — the wrap axis, whose
  // archive only a `culebra wrap`-extended driver embeds.
  bool feature_failed = false;
  auto force_load_feature = [&](const char* archive,
                                bool optional = false) -> std::string {
    std::string err;
    auto path = materialize_archive(archive, err);
    if (path.empty()) {
      if (optional) return "";
      std::println(stderr, "culebra build: {}", err);
      feature_failed = true;
      return "";
    }
    auto q = shq(path.string());   // the cache path can hold spaces on any OS
    return target_is_macho
        ? std::format("-Wl,-force_load,{}", q)
        : std::format("-Wl,--whole-archive {} -Wl,--no-whole-archive", q);
  };

  // Every force-load fragment precedes every plain link flag on the command
  // line, so collect the two runs separately and splice them in below. A
  // --rt-lib build gets the flags but none of the embedded objects: the archive
  // it points at is expected to carry the feature bodies itself.
  bool host_build = opts.rt_lib.empty() && !cross;
  std::vector<std::string> feature_objs, feature_links;
  for (size_t i = 0; i < std::size(kFeatureAxes); i++) {
    if (!used[i]) continue;
    const auto& ax = kFeatureAxes[i];
    if (host_build && ax.embedded)
      feature_objs.push_back(force_load_feature(ax.archive));
    feature_links.push_back(ax.link_flags);
  }
  // The wrap archive holds static registrars nothing references by name, so it
  // needs the same force-load for the binding to register at all; its flags are
  // whatever `culebra wrap --link` baked in. A wrapped library whose shared
  // objects need libz has to name -lz among them, for the reason the Webview
  // axis does (CMakeLists, _webview_link).
  if (host_build && uses_wrap)
    feature_objs.push_back(force_load_feature("libculebra_rt_wrap.a", true));
  if (uses_wrap) feature_links.push_back(CULEBRA_WRAP_LINK_FLAGS);
  if (feature_failed) return 1;

  std::string libcxx = target_is_macho ? "-lc++" : "-lstdc++ -lm";
  // LLVM's TargetMachine emits a non-PIC object by default. Modern
  // Linux distros (Ubuntu, Fedora) configure their `cc` to link as a
  // PIE executable unconditionally, which then refuses the non-PIC
  // .o with `failed to set dynamic section sizes: bad value`.
  // Force `-no-pie` on Linux to match the object's non-PIC reloc model
  // (distros default `cc` to PIE, which rejects the non-PIC .o). macOS
  // clang/ld take the PIE choice from the .o; Windows PE is always
  // relocatable and mingw doesn't want `-no-pie` here.
#ifdef _WIN32
  const char* no_pie = "";
  // Standalone .exe on a bare Windows (no mingw DLLs), matching the
  // interpreter build's link flags. `-lstdc++exp` resolves C++23 std::print's
  // __open_terminal / __write_to_terminal, which live in libstdc++exp (NOT in
  // -static-libstdc++); the runtime archive's `culebra_runtime_print` uses
  // std::print, so the AOT'd binary needs it too. Placed before the driver's
  // implicit -lstdc++ so the experimental lib resolves against the base.
  // ws2_32 mirrors the driver link (see CMakeLists WIN32 block): net.h reaches
  // the base runtime archive via stdlib_interp.h, so even a socket-free program
  // pulls Winsock out of libculebra_rt.a.
  // CULEBRA_SSL_LINK rides along for the same reason, one step further. The
  // core archive's Http entry points are weak stubs (CULEBRA_RT_HTTP_REQUEST_-
  // WEAK), but httplib's Windows-only TLS code -- schannel cert verification,
  // the SSL stream -- still lands in that object, so the archive carries
  // undefined OpenSSL/zlib/crypt32 references a non-Http program never calls.
  // ELF and Mach-O dead-strip those sections; ld for PE diagnoses the
  // references before --gc-sections runs, so Windows cannot gate this on use
  // and every AOT binary pays the ~4 MB. Duplicated on the link line when the
  // Http axis also fires, which the linker dedupes.
  std::string win_static =
      "-static -static-libgcc -static-libstdc++ -lstdc++exp -lws2_32";
  if (*CULEBRA_SSL_LINK) {
    win_static += ' ';
    win_static += CULEBRA_SSL_LINK;
  }
#else
  const char* no_pie = target_is_macho ? "" : "-no-pie";
  std::string win_static;
#endif

  // Assembled in link order: driver, target selection, inputs, force-loaded
  // feature objects, link-wide flags, then the libraries those objects need.
  // Empty parts (a flag that doesn't apply, a feature built out) drop out at
  // the join rather than leaving runs of blanks in the command.
  std::vector<std::string> parts{cc};
  if (cross) parts.push_back(std::format("--target={}", shq(opts.target)));
  if (!opts.sysroot.empty())
    parts.push_back(std::format("--sysroot={}", shq(opts.sysroot)));
  parts.push_back(shq(obj));
  parts.push_back(assets_obj);
  parts.push_back(shq(lib));
  parts.insert(parts.end(), feature_objs.begin(), feature_objs.end());
  parts.push_back(dead_strip);
  parts.push_back(strip_syms);
  parts.push_back(no_pie);
  parts.push_back(win_static);
  parts.push_back(libcxx);
  parts.insert(parts.end(), feature_links.begin(), feature_links.end());
  parts.push_back("-o");
  parts.push_back(shq(opts.output));

  std::string cmd;
  for (const auto& p : parts) {
    if (p.empty()) continue;
    if (!cmd.empty()) cmd += ' ';
    cmd += p;
  }

  if (verbose) std::println(stderr, "culebra build: link: {}", cmd);
  int link_rc = std::system(cmd.c_str());
  if (link_rc != 0) {
    std::println(stderr, "culebra build: link failed (rc={})", link_rc);
    return 1;
  }

  return 0;
}

// `culebra wrap decl.cpp [decl2.cpp ...] [-o out] [--link "<flags>"] [--lto]`
// — build an EXTENDED culebra binary with the wrap.h declarations compiled
// in (P3 / Phase 4). The "codegen" already happened in
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

  // The source tree to rebuild. `wrap` needs a buildable tree, not just the
  // headers `build` needs, so it checks for CMakeLists.txt.
  string src_dir = culebra::resolved_source_dir();
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

  bool verbose = std::getenv("CULEBRA_VERBOSE") != nullptr;
  auto configure = std::format(
      "cmake -S {} -B {} -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=ON "
      "-DCULEBRA_LTO={} -DCULEBRA_WRAP_SOURCES={} -DCULEBRA_WRAP_LINK={}{}",
      shq(src_dir), shq(build_dir.string()), opts.lto ? "ON" : "OFF",
      shq(wrap_sources), shq(opts.link_flags),
      verbose ? "" : std::format(" > {}", kNullDevice));
  if (verbose) std::println(stderr, "culebra wrap: configure: {}", configure);
  if (std::system(configure.c_str()) != 0) {
    std::println(stderr, "culebra wrap: cmake configure failed");
    return 1;
  }
  auto build = std::format("cmake --build {} --target culebra -j{}{}",
                           shq(build_dir.string()),
                           std::thread::hardware_concurrency(),
                           verbose ? "" : std::format(" > {}", kNullDevice));
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
      if (arg == "--version") { options.version = true; continue; }
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
        if (!parse_opt_level(arg, options.opt_level, options.error)) return options;
        options.opt_level_explicit = true;
        continue;
      }
#endif
    }
    options.script_path = std::move(arg);
    seen_script = true;
  }

  if (!options.shell) {
    options.shell = !options.script_path.has_value();
  }

  return options;
}

bool run_scripts(shared_ptr<culebra::Environment> env, const Options& options) {
  // Time / Args are registered lazily by `environment()` — no eager
  // preamble load needed here. The JIT path still pre-concats the
  // preamble (handled in Phase 3).
  startup_profile::mark("run_scripts begin");

  // Cooperative Ctrl+C: install the SIGINT handler and point this thread's
  // Runtime at the global flag. The interpreter's statement poll and the JIT's
  // loop safepoint observe it and throw a catchable `Interrupted`.
  if (options.script_path) culebra::install_sigint_handler();

  if (!options.script_path) return true;
  const string& path = *options.script_path;

  auto user_src = read_file(path.c_str());
  if (!user_src) {
    std::println(stderr, "can't open '{}'.", path);
    return false;
  }
  startup_profile::mark("read_file");

  // Walk the dependency graph via ModuleLoader. The same vector feeds both
  // backends — JIT bundles every module into one IR, interp evaluates them
  // sequentially — but only the JIT path needs the preamble spliced in.
  // --ast never reaches either, and the splice would make the dump differ by
  // backend for one program.
  bool splice = false;
#ifdef CULEBRA_JIT_ENABLED
  splice = options.jit && !options.print_ast;
#endif
  std::vector<culebra::LoadedModule> modules;
  if (!load_entry_program(path, *user_src, splice, modules)) return false;
  startup_profile::mark("ModuleLoader::load_program (parse)");

  // "Print the parsed AST instead of running it" — dump and stop.
  if (options.print_ast) {
    for (const auto& m : modules) {
      cout << "// " << m.abs_path.string() << "\n";
      cout << peg::ast_to_s(m.ast);
    }
    return true;
  }

#ifdef CULEBRA_JIT_ENABLED
  if (options.jit) {
    culebra::JIT::run_modules(modules, options.emit_llvm, options.debug,
                               options.opt_level, options.jit_faststart);
    return true;
  }
#endif

  culebra::Value val;
  vector<string> run_msgs;
  auto dbg =
      options.debug ? culebra::CommandLineDebugger() : culebra::Debugger();
  if (culebra::interpret_modules(modules, env, val, run_msgs, dbg)) {
    startup_profile::mark("interpret_modules");
    return true;
  }
  for (const auto& msg : run_msgs) cerr << msg << endl;
  return false;
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
  // `--bail`'s count is optional, so the parser has to tell a count from the
  // next path. Digits only: std::stoi stopped at the first non-digit, which
  // made `--bail 3rd_party/` mean "bail after 3" and swallow the path.
  auto all_digits = [](std::string_view v) {
    return !v.empty() &&
           v.find_first_not_of("0123456789") == std::string_view::npos;
  };
  auto parse_bail = [&](std::string_view v) {
    int n = 0;
    if (all_digits(v)) {
      try {
        n = std::stoi(std::string(v));   // digits checked; this catches overflow
      } catch (...) {
        n = 0;
      }
    }
    if (n < 1) {
      std::println(stderr,
                   "culebra test: --bail needs a count of 1 or more, got '{}'",
                   v);
      return false;
    }
    bail_after = n;
    return true;
  };
  for (int i = 2; i < argc; i++) {
    std::string arg(argv[i]);
    if (arg == "-h" || arg == "--help") {
      std::println("Usage: culebra test [options] [paths...]\n"
                   "  (no flags)      discover test_*.cul and run every test\n"
                   "  --filter <s>    run only tests whose name contains <s>\n"
                   "  --reporter <r>  default (human) or json (NDJSON events)\n"
                   "  --bail [n]      stop after the first failure, or after n\n"
                   "  --list          print the discovered test names, run none\n"
                   "  --doc           run the ```culebra blocks in *.md instead\n"
                   "  paths           files, or directories scanned recursively");
      return 0;
    }
    if (arg.starts_with("--filter=")) {
      filter = arg.substr(9);
    } else if (arg == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    } else if (arg.starts_with("--reporter=")) {
      if (!parse_reporter(arg.substr(11))) return 2;
    } else if (arg == "--reporter" && i + 1 < argc) {
      if (!parse_reporter(argv[++i])) return 2;
    } else if (arg == "--bail") {
      // `--bail` alone means 1; anything but a count after it is a path.
      if (i + 1 < argc && all_digits(argv[i + 1])) {
        if (!parse_bail(argv[++i])) return 2;
      } else {
        bail_after = 1;
      }
    } else if (arg.starts_with("--bail=")) {
      if (!parse_bail(arg.substr(7))) return 2;
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
      auto e = culebra::environment();
      install_cli_aliases(*e);
      culebra::install_doctest_exit_guard(*e);
      return e;
    };
    summary = culebra::run_doctests(
        files, filter, make_env, reporter, bail_after, list_only);
  } else {
    auto env = culebra::environment();
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

// Expand any directory argument into the `.cul` files it contains (recursively,
// sorted for stable output); plain file arguments pass through unchanged. A
// directory that can't be read is reported via `ok=false`. Shared by `fmt`
// and `lint` (both take `[paths...]` where a path may be a directory).
static vector<string> expand_cul_paths(const string& tool,
                                       const vector<string>& args, bool& ok) {
  namespace fs = std::filesystem;
  vector<string> out;
  for (const auto& a : args) {
    std::error_code ec;
    if (!fs::is_directory(a, ec)) { out.push_back(a); continue; }
    vector<string> found;
    for (fs::recursive_directory_iterator it(a, ec), end; it != end; it.increment(ec)) {
      if (ec) { std::println(stderr, "culebra {}: can't read '{}'", tool, a); ok = false; break; }
      if (it->is_regular_file(ec) && it->path().extension() == ".cul")
        found.push_back(it->path().string());
    }
    std::sort(found.begin(), found.end());
    out.insert(out.end(), found.begin(), found.end());
  }
  return out;
}

// Remove every source line named in `targets` (1-based line numbers) from
// `src`, preserving every other line's exact bytes (including whether the
// file ends with a trailing newline). Used by `culebra lint --fix` to drop
// dead `import` statements, which are ordinary single-line top-level
// statements (`import _ IDENTIFIER _ from _ STRING`, see grammar_def.h) in
// the style the corpus and `culebra fmt` both produce. The grammar does
// allow `;`-joining two statements onto one physical line, so a dead import
// can share a line with something else — the caller passes only the lines
// `lint::removable_import_lines` cleared as belonging to one import and
// nothing else, which is what keeps a line-wise delete honest.
static std::string remove_source_lines(const std::string& src,
                                       const std::set<long>& targets) {
  std::string out;
  long line_no = 1;
  size_t i = 0;
  while (i <= src.size()) {
    size_t nl = src.find('\n', i);
    size_t end = nl == std::string::npos ? src.size() : nl + 1;
    if (!targets.contains(line_no)) out.append(src, i, end - i);
    if (nl == std::string::npos) break;
    i = end;
    line_no++;
  }
  return out;
}

// `culebra lint [paths...]` — parse each file and report static
// diagnostics (the load-stage errors plus advisory warnings like unused
// locals) without evaluating. Each file is linted on its own (imports bind
// their namespace name, so single-file analysis is sound). A directory
// argument is expanded to the `.cul` files under it (recursively). Exit
// code: 0 when clean, 1 when only warnings were found, 2 when any error (or
// a parse / read failure) occurred — so CI can gate on it.
//
// `--fix` mechanically removes unused-import lines (`UnusedImport`
// diagnostics). It is the only warning kind scoped to autofix: deleting a
// dead `import` line can never change behavior (an import has no
// author-visible side effect beyond binding the name), whereas deleting an
// unused top-level `let`/`mut` could drop a side-effecting initializer
// expression — unsafe to do unattended. Every other warning kind is
// report-only. After editing, the fixed source is re-parsed and re-linted;
// the fix is written only if that re-check confirms the removed imports are
// gone and no new error-severity diagnostic appeared (mirrors `culebra
// fmt`'s re-parse safety net — never write a change that wasn't
// independently verified).
int run_lint(int argc, const char** argv) {
  bool fix = false;
  vector<string> files;
  for (int i = 2; i < argc; i++) {
    string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::println("Usage: culebra lint [--fix] <paths...>\n"
                   "  (no flags)  report static problems, exit 0/1/2\n"
                   "  --fix       mechanically remove unused-import lines\n"
                   "  paths       files, or directories scanned for *.cul");
      return 0;
    }
    if (arg == "--fix") { fix = true; continue; }
    // A misspelled flag must not fall through to the path list: linting the
    // remaining files while the flag the user asked for silently did nothing
    // is the worst of both outcomes. (A path really named `-x` is reachable
    // as `./-x`.)
    if (arg.size() > 1 && arg[0] == '-') {
      std::println(stderr, "culebra lint: unknown option '{}'", arg);
      return 2;
    }
    files.push_back(arg);
  }
  if (files.empty()) {
    std::println(stderr, "culebra lint: no input files");
    return 2;
  }
  bool expand_ok = true;
  files = expand_cul_paths("lint", files, expand_ok);
  // Directories that hold no .cul files would otherwise report a clean run.
  if (files.empty()) {
    std::println(stderr, "culebra lint: no *.cul files in the given paths");
    return 2;
  }

  int errors = 0, warnings = 0;
  bool had_failure = !expand_ok;
  for (const auto& path : files) {
    auto contents = read_file(path.c_str());
    if (!contents) {
      std::println(stderr, "culebra lint: can't open '{}'", path);
      had_failure = true;
      continue;
    }
    std::string src = std::move(*contents);
    // `collect_module` wants both views of the program: the lowered AST the
    // backends run (sound error checks) and the source as written (advisory
    // warnings). See its comment for why neither alone is enough.
    auto lint_source = [&](const std::string& s, vector<string>& parse_msgs,
                           std::shared_ptr<peg::Ast>* authored_out = nullptr)
        -> std::optional<vector<culebra::lint::Diagnostic>> {
      auto authored = culebra::parse(path, s.data(), s.size(), parse_msgs);
      if (!authored) return std::nullopt;
      if (authored_out) *authored_out = authored;
      // The lowering itself rejects malformed effects (two `return` clauses,
      // a duplicate handler clause, …) by throwing. Report those as ordinary
      // error diagnostics instead of letting them escape the CLI — a linter
      // must never abort on the input it was asked to inspect.
      std::shared_ptr<peg::Ast> lowered;
      try {
        lowered = culebra::parse_with_transforms(path, s.data(), s.size(),
                                                 parse_msgs);
      } catch (const culebra::CulebraError& e) {
        return vector<culebra::lint::Diagnostic>{
            {e.kind, e.what(), e.line, e.col, culebra::lint::Severity::Error}};
      }
      if (!lowered) return std::nullopt;
      return culebra::lint::collect_module(*lowered, *authored);
    };

    vector<string> parse_msgs;
    std::shared_ptr<peg::Ast> authored;
    auto diags = lint_source(src, parse_msgs, &authored);
    if (!diags) {
      for (const auto& m : parse_msgs) std::print(stderr, "{}", m);
      had_failure = true;
      continue;
    }

    if (fix) {
      // Only lines that hold one import and nothing else can be deleted
      // wholesale; an import sharing its line stays reported but untouched.
      auto removable = culebra::lint::removable_import_lines(*authored);
      std::set<long> import_lines;
      int shared_lines = 0;
      for (const auto& d : *diags)
        if (d.kind == "UnusedImport") {
          if (removable.contains(d.line))
            import_lines.insert(d.line);
          else
            shared_lines++;
        }
      if (shared_lines > 0)
        std::println(stderr,
                     "{}: --fix skipped {} unused import{} sharing a line with "
                     "other code",
                     path, shared_lines, shared_lines == 1 ? "" : "s");
      if (!import_lines.empty()) {
        auto fixed_src = remove_source_lines(src, import_lines);
        vector<string> fix_msgs;
        auto fixed_diags = lint_source(fixed_src, fix_msgs);
        bool verified = fixed_diags.has_value();
        if (verified) {
          int orig_errors = 0, new_errors = 0;
          for (const auto& d : *diags)
            if (d.severity == culebra::lint::Severity::Error) orig_errors++;
          for (const auto& d : *fixed_diags) {
            if (d.severity == culebra::lint::Severity::Error) new_errors++;
            if (d.kind == "UnusedImport") verified = false;
          }
          verified = verified && new_errors == orig_errors;
        }
        if (verified) {
          ofstream ofs(path, ios::out | ios::binary | ios::trunc);
          ofs << fixed_src;
          ofs.close();   // close before the check: flushing is where ENOSPC lands
          if (!ofs) {
            std::println(stderr, "culebra lint: can't write '{}'", path);
            had_failure = true;   // the file still holds the unfixed source
          } else {
            std::println("{}: fixed {} unused import{}", path,
                        import_lines.size(), import_lines.size() == 1 ? "" : "s");
            src = std::move(fixed_src);
            diags = std::move(fixed_diags);
          }
        } else {
          std::println(stderr,
                      "{}: --fix left {} unused import{} unresolved "
                      "(re-check failed, left unchanged)",
                      path, import_lines.size(),
                      import_lines.size() == 1 ? "" : "s");
        }
      }
    }

    for (const auto& d : *diags) {
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

// `culebra fmt [paths...]` — reformat Culebra source to the canonical style.
// Default: write the formatted result to stdout. `-i`/`--in-place` rewrites
// files in place. `-l`/`--list` prints the names of files whose formatting
// differs (and exits 1). `--check` is like `-l` but prints nothing. The three
// compose — `-i -l` rewrites and names what it rewrote, like `gofmt -l -w`. A
// directory argument is expanded to the `.cul` files under it (recursively).
// With no paths (or `-`) it formats stdin to stdout (for editor
// format-on-save); `-` is exclusive, since neither a second input nor an
// in-place rewrite has a meaning there. Exit code: 0 clean, 1 when
// `-l`/`--check` found changes, 2 on a bad argument or a parse / read / safety
// failure.
int run_fmt(int argc, const char** argv) {
  bool in_place = false, list = false, check = false, has_stdin = false;
  vector<string> files;
  for (int i = 2; i < argc; i++) {
    string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::println("Usage: culebra fmt [-i|--in-place] [-l|--list] [--check] "
                   "[paths...]\n"
                   "  (no flags)      write formatted source to stdout\n"
                   "  -i, --in-place  rewrite each file in place\n"
                   "  -l, --list      list files whose formatting differs "
                   "(exit 1)\n"
                   "  --check         exit 1 if any file's formatting differs "
                   "(no output)\n"
                   "  paths           files, or directories scanned for *.cul\n"
                   "  -               read from stdin, write to stdout "
                   "(no paths, no -i)");
      return 0;
    }
    if (arg == "-i" || arg == "--in-place") { in_place = true; continue; }
    if (arg == "-l" || arg == "--list") { list = true; continue; }
    if (arg == "--check") { check = true; continue; }
    // `-` selects stdin as the input; it is not a path to open or expand.
    if (arg == "-") { has_stdin = true; continue; }
    // See run_lint: an unknown flag is an error, not a file name.
    if (!arg.empty() && arg[0] == '-') {
      std::println(stderr, "culebra fmt: unknown option '{}'", arg);
      return 2;
    }
    files.push_back(arg);
  }

  // Two inputs and one stdout has no honest reading, so refuse the mix instead
  // of letting either side quietly win.
  if (has_stdin && !files.empty()) {
    std::println(stderr, "culebra fmt: can't mix '-' (stdin) with file paths");
    return 2;
  }
  // Decided before expansion, while the answer is still known: a directory
  // holding no `.cul` files expands to nothing, which has to stay the reported
  // mistake below instead of decaying into the no-arguments case and blocking
  // on stdin.
  bool use_stdin = has_stdin || files.empty();
  if (use_stdin && in_place) {
    std::println(stderr, "culebra fmt: -i needs file paths; stdin has nothing "
                         "to rewrite");
    return 2;
  }

  bool expand_ok = true;
  if (!use_stdin) {
    files = expand_cul_paths("fmt", files, expand_ok);
    // Named paths that hold no `.cul` file are a mistake, not a clean run.
    if (files.empty()) {
      std::println(stderr, "culebra fmt: no *.cul files in the given paths");
      return 2;
    }
  }

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

  if (use_stdin) {
    std::string src((std::istreambuf_iterator<char>(cin)),
                    std::istreambuf_iterator<char>());
    auto r = fmt("<stdin>", src);
    report("<stdin>", r);
    if (r.status == culebra::fmt::FormatStatus::ParseError ||
        r.status == culebra::fmt::FormatStatus::Refused)
      return 2;
    bool changed = r.status == culebra::fmt::FormatStatus::Ok;
    if (list && changed) std::println("<stdin>");
    if (!list && !check) std::print("{}", r.output);
    return (list || check) && changed ? 1 : 0;
  }

  int rc = expand_ok ? 0 : 2;
  bool any_changed = false;
  for (const auto& path : files) {
    auto src = read_file(path.c_str());
    if (!src) {
      std::println(stderr, "culebra fmt: can't open '{}'", path);
      rc = 2;
      continue;
    }
    auto r = fmt(path, *src);
    report(path, r);
    if (r.status == culebra::fmt::FormatStatus::ParseError ||
        r.status == culebra::fmt::FormatStatus::Refused) {
      rc = 2;
      continue;
    }
    bool changed = r.status == culebra::fmt::FormatStatus::Ok;
    if (changed) any_changed = true;
    // The modes compose: `-l` names what changed, `-i` writes it, stdout is
    // the fallback when neither reporting flag was given.
    if (list && changed) std::println("{}", path);
    if (in_place) {
      if (changed) {
        ofstream ofs(path, ios::out | ios::binary | ios::trunc);
        ofs << r.output;
        ofs.close();   // close before the check: flushing is where ENOSPC lands
        if (!ofs) {
          std::println(stderr, "culebra fmt: can't write '{}'", path);
          rc = 2;
        }
      }
    } else if (!list && !check) {
      std::print("{}", r.output);
    }
  }
  if ((check || list) && any_changed && rc == 0) return 1;
  return rc;
}

int main(int argc, const char** argv) {
  startup_profile::start();
  startup_profile::mark("main entered");

  // Before anything can print.
  culebra::install_console_utf8();

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
  if (argc >= 2 && string(argv[1]) == "docs") {
    return culebra::run_docs(argc, argv, CULEBRA_VERSION);
  }
  if (argc >= 2 && string(argv[1]) == "init") {
    return culebra::run_init(argc, argv);
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

  if (!options.error.empty()) {
    std::println(stderr, "culebra: {}", options.error);
    return 1;
  }
  if (options.help) {
    print_usage(cout);
    return 0;
  }
  if (options.version) {
    // Name the backends the build actually has: a no-JIT build cannot run
    // --jit or `culebra build`, so a bug report needs to say which binary.
#ifdef CULEBRA_JIT_ENABLED
    constexpr auto backends = "interp+jit";
#else
    constexpr auto backends = "interp";
#endif
    std::println(cout, "culebra {} ({})", CULEBRA_VERSION, backends);
    return 0;
  }

#ifdef CULEBRA_JIT_ENABLED
  // --jit-faststart names one configuration, not a backend knob that can be
  // mixed with an arbitrary IR level: an optimized IR pipeline feeding the
  // unoptimized backend hits an LLVM register-allocation bug that silently
  // corrupts values on throw paths (JIT::apply_fast_codegen). Say so rather
  // than quietly running something the user did not ask for.
  if (options.jit_faststart && options.opt_level_explicit &&
      options.opt_level != 0) {
    std::println(cerr,
                 "culebra: --jit-faststart implies -O0; drop the -O{} (or "
                 "use --jit -O{} for the optimizing backend)",
                 options.opt_level, options.opt_level);
    return 1;
  }
  if (options.jit_faststart) options.opt_level = 0;

  culebra::install_jit_stdlib();
  startup_profile::mark("install_jit_stdlib");
#endif

  // `Sys.script` — the entry script's absolute path, baked into the Sys
  // namespace when it is built (below). Set before `environment()` so the
  // interpreter sees it; the JIT reads the same holder when it materializes
  // Sys. Its directory is the base for `Embed.dir(...)`'s disk fallback (dev),
  // so embedded-asset paths resolve the way the AOT build walks them —
  // relative to the source — whatever the cwd is. Both are left empty (→ nil)
  // for the REPL and stdin.
  culebra::set_main_script("");
  if (options.script_path) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(*options.script_path, ec);
    if (!ec) culebra::set_main_script(abs.string());
  }

  // `Sys.argv` — the arguments after the script path. Set here, beside the
  // script path, because both backends and every thread read it from the same
  // process-wide holder (the AOT bootstrap fills it in the same way).
  culebra::sys_argv() = options.script_argv;

  try {
    auto env = culebra::environment();
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
    print_culebra_error(e);
    return -1;
  } catch (const exception& e) {
    cerr << e.what() << endl;
    return -1;
  }

  return 0;
}
