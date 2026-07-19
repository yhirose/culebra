// culebra playground — interp-only core compiled to WebAssembly.
// Call-based (no main): JS calls run_culebra(src) then get_output().
// stdout (IO.puts/print write to std::cout) is captured deterministically by
// swapping std::cout's stream buffer, so trailing newline-less output from
// `print` is never lost to emscripten's line buffering.
#include <interpreter.h>
#include <module_loader.h>
#include <stdlib_interp.h>

#include <emscripten/emscripten.h>

#include <sstream>
#include <string>
#include <vector>

static std::string g_output;

extern "C" {

// Run one program; returns 0 on success, 1 on error. Combined stdout + error
// text is retrievable via get_output().
EMSCRIPTEN_KEEPALIVE int run_culebra(const char* src_c) {
  std::ostringstream cap;
  std::streambuf* old = std::cout.rdbuf(cap.rdbuf());
  std::streambuf* old_err = std::cerr.rdbuf(cap.rdbuf());  // IO.eputs/eprint

  int rc = 0;
  std::vector<std::string> msgs;
  culebra::ModuleLoader loader;
  std::vector<culebra::LoadedModule> modules;
  try {
    modules = loader.load_program("<playground>", src_c, msgs);
    if (modules.empty()) {
      for (auto& m : msgs) cap << m << "\n";
      rc = 1;
    } else {
      auto env = culebra::environment({});
      culebra::install_cli_aliases(*env);
      culebra::Value val;
      culebra::Debugger dbg;
      if (!culebra::interpret_modules(modules, env, val, msgs, dbg)) {
        for (auto& m : msgs) cap << m << "\n";
        rc = 1;
      }
    }
  } catch (const culebra::CulebraError& e) {
    cap << e.kind << ": " << e.what();
    if (e.line > 0 || e.col > 0) cap << " at " << e.line << ":" << e.col << ".";
    cap << "\n";
    rc = 1;
  } catch (const std::exception& e) {
    cap << "error: " << e.what() << "\n";
    rc = 1;
  }

  std::cout.rdbuf(old);
  std::cerr.rdbuf(old_err);
  g_output = std::move(cap).str();
  return rc;
}

EMSCRIPTEN_KEEPALIVE const char* get_output() { return g_output.c_str(); }

}  // extern "C"
