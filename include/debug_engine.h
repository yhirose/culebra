#pragma once

// The seam a source-level debugger sits on.
//
// Everything a debugger does above the engine — the DAP wire protocol,
// breakpoint tables, the pause/resume state machine, forwarding the
// debuggee's output — is the same whichever engine runs the program. What is
// not the same is the handful of questions below: where the statement
// boundaries are, what the call stack looks like, which names a frame can
// see, and what an expression evaluated against one means. The bytecode VM
// answers them from its chunks' debug tables.
//
// Every method except `run` is called while the program is parked inside the
// stop callback, ON THE THREAD `run` WAS CALLED ON. That is a requirement,
// not a convenience: a frame's state belongs to the thread executing it (the
// VM keeps registers on that thread's machine stack, and the Runtime and GC
// are per-thread).

#include <culebra.h>
#include <debug_types.h>
#include <module_loader.h>
#include <stdlib_preamble.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <stdlib_jit.h>
#include <vm.h>
#include <vm_debug.h>

namespace culebra {

// One statement boundary. `force` marks the `debugger` statement, which
// breaks whether or not anything asked to stop here; `depth` is the engine's
// own notion of how deep this point is, which step over / step out compare
// against the depth they were requested at.
using DebugStop = std::function<void(const std::string& path, int64_t line,
                                     bool force, size_t depth)>;
using DebugDiag = std::function<void(const std::string&)>;

class DebugEngine {
 public:
  virtual ~DebugEngine() = default;

  // Loads and runs `program`, calling `stop` at every statement boundary.
  // Returns the process exit code; diagnostics go to `diag`.
  virtual int run(const std::string& program,
                  const std::vector<std::string>& argv, const DebugStop& stop,
                  const DebugDiag& diag) = 0;

  // The call stack, innermost first. Frame ids are 1-based indices into it.
  virtual std::vector<DebugFrame> frames() = 0;
  virtual std::vector<DebugVar> variables(size_t frame_ix) = 0;
  virtual bool has_name(size_t frame_ix, const std::string& name) = 0;
  virtual bool evaluate(size_t frame_ix, const std::string& expr,
                        DebugVar& out, std::string& err) = 0;
  virtual bool set_variable(size_t frame_ix, const std::string& name,
                            const std::string& expr, DebugVar& out,
                            std::string& err) = 0;
};

// ---------------------------------------------------------------------------
// The bytecode VM's executor
//
// The lowering lane gets no debugger, for the reason it gets no REPL: a
// debug session reads register windows and a frame stack that native code
// does not keep, and stepping is a tier-0 activity anyway (include/repl.h).
// ---------------------------------------------------------------------------

class VmDebugEngine : public DebugEngine {
 public:
  int run(const std::string& program, const std::vector<std::string>& argv,
          const DebugStop& stop, const DebugDiag& diag) override {
    // `culebra dap` is dispatched before main's install_jit_stdlib(), so a
    // compiled lane installs its own — without it `__Eff` / `SQLite` /
    // `__Foreign` are unresolved (the doctest runner's lane learned this).
    culebra::install_jit_stdlib();
    std::ifstream ifs(program, std::ios::binary);
    std::string src((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
    if (!ifs && src.empty()) {
      diag("cannot open '" + program + "'\n");
      return 1;
    }
    std::vector<std::string> msgs;
    std::vector<LoadedModule> modules;
    try {
      ModuleLoader loader;
      modules = loader.load_program(program, src, msgs);
      splice_stdlib_preamble(modules);
    } catch (const CulebraError& e) {
      // interrupt: loading polls nothing, so none arrives here.
      diag(format_error_message(e) + "\n");
      return 1;
    }
    if (modules.empty()) {
      for (const auto& m : msgs) diag(m + "\n");
      return 1;
    }
    culebra::sys_argv() = argv;
    auto& st = vm::dbg_state();
    st.tracking = true;
    st.hook = [&](bool force, int64_t line, int64_t /*col*/) {
      stop(prog_ ? prog_->source_path : program, line, force,
           vm::dbg_state().frames.size());
    };
    int code = 0;
    try {
      // The debug lane takes a single script (plus the spliced stdlib
      // preamble); real dependencies stay unsupported here.
      bool has_preamble =
          modules.front().abs_path == culebra::kStdlibPreamblePath;
      if (modules.size() > (has_preamble ? 2u : 1u)) {
        throw CulebraError("VmError", "--vm: unsupported: multi-module script");
      }
      prog_ = std::make_unique<vm::VmProgram>(
          vm::Compiler::compile_modules(modules, vm::Debug::Step));
      vm::Exec::run(*prog_);
    } catch (const CulebraError& e) {
      // interrupt: the DAP lane installs no SIGINT handler, so no press reaches
      // the engine here — and this runs on the debuggee thread, where letting
      // anything escape is std::terminate. If the lane ever polls, report the
      // interrupt to the client; do not re-throw it off this thread.
      diag(format_error_message(e) + "\n");
      code = 1;
    } catch (const std::exception& e) {
      // Exec::run has already turned an uncaught user throw into the
      // "uncaught: ..." line every backend prints for one.
      diag(std::string(e.what()) + "\n");
      code = 1;
    } catch (...) {
      code = 1;
    }
    st.hook = nullptr;
    st.tracking = false;
    return code;
  }

  std::vector<DebugFrame> frames() override { return session_.frames(); }

  std::vector<DebugVar> variables(size_t frame_ix) override {
    return session_.variables(frame_ix);
  }

  bool has_name(size_t frame_ix, const std::string& name) override {
    return session_.has_name(frame_ix, name);
  }

  bool evaluate(size_t frame_ix, const std::string& expr, DebugVar& out,
                std::string& err) override {
    return session_.evaluate(frame_ix, expr, out, err);
  }

  bool set_variable(size_t frame_ix, const std::string& name,
                    const std::string& expr, DebugVar& out,
                    std::string& err) override {
    return session_.set_variable(frame_ix, name, expr, out, err);
  }

 private:
  std::unique_ptr<vm::VmProgram> prog_;
  VmDebugSession session_;
};

inline std::unique_ptr<DebugEngine> make_debug_engine() {
  return std::make_unique<VmDebugEngine>();
}

}  // namespace culebra
