// culebra playground — interp-only core compiled to WebAssembly.
// Call-based (no main): JS calls run_culebra(src).
//
// Output streams live via a custom std::streambuf that posts to JS the
// moment something flushes std::cout/cerr, rather than being captured and
// handed back only after run_culebra returns. That capture-and-return
// design (the previous approach here) works for a quick script but breaks a
// TUI one: Term.app's event loop doesn't return until the user quits, so
// nothing would ever reach the page while a game is being played. IO.inspect
// (which ends in std::endl) and Screen.flush() (one TUI frame) already
// flush, so streaming falls out of the existing stdlib for free — no interp
// change needed. IO.print alone doesn't auto-flush; run_culebra flushes
// once more at the end so a plain script's trailing print()-without-\n text
// is never lost, matching the old behavior for non-TUI scripts.
#include <interpreter.h>
#include <module_loader.h>
#include <stdlib_interp.h>

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
EMSCRIPTEN_KEEPALIVE int run_culebra(const char* src_c) {
  (void)g_tensor_auto;
  (void)g_streams_installed;

  int rc = 0;
  std::vector<std::string> msgs;
  culebra::ModuleLoader loader;
  std::vector<culebra::LoadedModule> modules;
  try {
    modules = loader.load_program("<playground>", src_c, msgs);
    if (modules.empty()) {
      for (auto& m : msgs) std::cerr << m << "\n";
      rc = 1;
    } else {
      auto env = culebra::environment({});
      culebra::install_cli_aliases(*env);
      culebra::Value val;
      culebra::Debugger dbg;
      if (!culebra::interpret_modules(modules, env, val, msgs, dbg)) {
        for (auto& m : msgs) std::cerr << m << "\n";
        rc = 1;
      }
    }
  } catch (const culebra::CulebraError& e) {
    std::cerr << e.kind << ": " << e.what();
    if (e.line > 0 || e.col > 0) std::cerr << " at " << e.line << ":" << e.col << ".";
    std::cerr << "\n";
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
