// culebra playground — the core compiled to WebAssembly, running on the
// bytecode VM's executor. There is no LLVM here, so `--jit`'s lowering is the
// one lane this build cannot have (docs/internals/vm.md §9).
// Call-based (no main): JS calls run_culebra(src).
//
// Output streams live via a custom std::streambuf that posts to JS the
// moment something flushes std::cout/cerr, rather than being captured and
// handed back only after run_culebra returns. That capture-and-return
// design (the previous approach here) works for a quick script but breaks a
// TUI one: Term.app's event loop doesn't return until the user quits, so
// nothing would ever reach the page while a game is being played. IO.inspect
// (which ends in std::endl) and Screen.flush() (one TUI frame) already
// flush, so streaming falls out of the existing stdlib for free — no engine
// change needed. IO.print alone doesn't auto-flush; run_culebra flushes
// once more at the end so a plain script's trailing print()-without-\n text
// is never lost, matching the old behavior for non-TUI scripts.
#include <module_loader.h>
#include <stdlib_jit.h>  // the stdlib the executor resolves through
#include <stdlib_preamble.h>
#include <vfs.h>
#include <vm.h>

#include <emscripten.h>
#include <emscripten/emscripten.h>

#include <string>
#include <vector>

class StreamingBuf : public std::streambuf {
 protected:
  int overflow(int ch) override {
    if (ch != EOF) buf_.push_back(static_cast<char>(ch));
    return ch;
  }
  std::streamsize xsputn(const char* s, std::streamsize n) override {
    buf_.append(s, static_cast<size_t>(n));
    return n;
  }
  int sync() override {
    if (!buf_.empty()) {
      EM_ASM({ postMessage({ type: "output", text: UTF8ToString($0, $1) }); },
             buf_.data(), buf_.size());
      buf_.clear();
    }
    return 0;
  }

 private:
  std::string buf_;
};

static StreamingBuf g_stream_buf;

// tensorlib starts in cpu mode; auto is what makes the WebGPU build worth
// shipping — it keeps small tensors on the CPU (a browser dispatch has a
// ~0.3-0.6 ms floor) and only pays for the GPU past the measured per-kernel
// crossovers. Harmless in the CPU build: with no backend compiled in,
// gpu_available() is false and auto always resolves to the CPU path.
static const bool g_tensor_auto = [] {
  culebra::tensor_use_auto();
  return true;
}();

static const bool g_streams_installed = [] {
  std::cout.rdbuf(&g_stream_buf);
  std::cerr.rdbuf(&g_stream_buf);  // IO.einspect/eprint
  return true;
}();

extern "C" {

// Run one program; returns 0 on success, 1 on error. Output (including
// error text) has already streamed to JS as "output" messages by the time
// this returns — see StreamingBuf above.
//
// `path_c` is where the program lives in the worker's in-memory filesystem,
// which worker.js mirrors from the repo layout. It does two jobs, and both are
// what let a program's source be byte-identical here and natively:
//   - it is the entry path the module loader resolves `import "./x.cul"`
//     against, so a multi-file program works;
//   - it becomes `Sys.script`, so a program finds its assets the same way in
//     both places (FS.dirname(Sys.script) + "/assets/...").
// Natively this is set from the command line in main.cc; leaving it unset here
// is why `Sys.script` used to read back as nil in the Playground.
//
// `args_c` becomes `Sys.argv`, newline-separated because a program's arguments
// here are written in examples.json rather than typed at a shell. It is what
// lets an example be tuned for the browser without forking its source: the
// wasm build has no JIT, so a program whose cost scales with a setting can be
// handed a smaller one and stay byte-identical to the file that runs natively.
EMSCRIPTEN_KEEPALIVE int run_culebra(const char* src_c, const char* path_c,
                                     const char* args_c) {
  (void)g_tensor_auto;
  (void)g_streams_installed;

  std::string path = (path_c && *path_c) ? path_c : "/work/main.cul";
  // Sets the entry directory too, which is what `Embed.dir(name)` resolves its
  // live-disk base against — the worker writes a program's assets next to its
  // source, at the same relative path, so a handle finds them here as well.
  culebra::set_main_script(path);

  std::vector<std::string> argv;
  if (args_c && *args_c) {
    std::string rest = args_c;
    size_t start = 0;
    while (start <= rest.size()) {
      size_t nl = rest.find('\n', start);
      if (nl == std::string::npos) nl = rest.size();
      if (nl > start) argv.push_back(rest.substr(start, nl - start));
      start = nl + 1;
    }
  }

  // Outside the Runtime below, so it lands in the process-wide default hooks
  // every per-run Runtime falls back to rather than in one run's copy.
  culebra::install_jit_stdlib();

  // A run is independent of every other, the way a doctest block is: the
  // namespace caches and the class / overload registries live in the Runtime,
  // and a cached namespace's closures point into the VmProgram that built it —
  // which this call owns and destroys. The page keeps one wasm instance for
  // every Run click, so a Runtime carried between them dangles on the second.
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);

  int rc = 0;
  std::vector<std::string> msgs;
  culebra::ModuleLoader loader;
  std::vector<culebra::LoadedModule> modules;
  try {
    modules = loader.load_program(path, src_c, msgs);
    if (modules.empty()) {
      for (auto& m : msgs) std::cerr << m << "\n";
      rc = 1;
    } else {
      culebra::sys_argv() = argv;
      // The preamble declares the lazy stdlib's builders; the compiled lanes
      // cannot resolve `Time` or `assert_eq` without it. Everything else
      // arrives as an exception the handlers below print.
      culebra::splice_stdlib_preamble(modules);
      culebra::vm::run_modules(modules);
    }
  } catch (const culebra::CulebraError& e) {
    std::cerr << culebra::format_error_message(e) << "\n";
    rc = 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    rc = 1;
  }

  std::cout.flush();
  std::cerr.flush();
  return rc;
}

}  // extern "C"
