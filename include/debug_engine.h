#pragma once

// The seam a source-level debugger sits on.
//
// Everything a debugger does above the engine — the DAP wire protocol,
// breakpoint tables, the pause/resume state machine, forwarding the
// debuggee's output — is the same whichever engine runs the program. What is
// not the same is the handful of questions below: where the statement
// boundaries are, what the call stack looks like, which names a frame can
// see, and what an expression evaluated against one means. The tree-walker
// answers them from its AST and environment chain; the bytecode VM answers
// them from its chunks' debug tables.
//
// Every method except `run` is called while the program is parked inside the
// stop callback, ON THE THREAD `run` WAS CALLED ON. That is a requirement,
// not a convenience: a frame's state belongs to the thread executing it (the
// VM keeps registers on that thread's machine stack, and both engines keep
// their Runtime and GC per-thread).

#include <culebra.h>
#include <debug_types.h>
#include <module_loader.h>
#include <stdlib_interp.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef CULEBRA_JIT_ENABLED
#include <stdlib_jit.h>
#include <vm.h>
#include <vm_debug.h>
#endif

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
// The tree-walker
// ---------------------------------------------------------------------------

class InterpDebugEngine : public DebugEngine {
 public:
  int run(const std::string& program, const std::vector<std::string>& argv,
          const DebugStop& stop, const DebugDiag& diag) override {
    std::ifstream ifs(program, std::ios::binary);
    std::string src((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
    if (!ifs && src.empty()) {
      diag("cannot open '" + program + "'\n");
      return 1;
    }
    std::vector<std::string> msgs;
    ModuleLoader loader;
    std::vector<LoadedModule> modules;
    try {
      modules = loader.load_program(program, src, msgs);
    } catch (const CulebraError& e) {
      diag(format_error_message(e) + "\n");
      return 1;
    }
    culebra::sys_argv() = argv;
    auto env = culebra::environment();
    // CLI-style global aliases so a script using bare inspect/print/println
    // behaves like a normal `culebra <file>` run.
    install_cli_aliases(*env);

    Debugger dbg = [&](const peg::Ast& a, Environment& e, bool force) {
      // An expression the debugger is evaluating is not a statement of the
      // program: its boundaries must not re-enter the hook that is holding
      // this thread. A fresh Interpreter is not enough on its own — a user
      // function's body runs through the Interpreter that *defined* it, which
      // is this one (the VM engine suppresses its hook for the same reason).
      if (evaluating_) return;
      cur_ast_ = &a;
      cur_env_ = &e;
      stop(a.path, static_cast<int64_t>(a.line), force, e.level);
      cur_ast_ = nullptr;
      cur_env_ = nullptr;
    };
    // Track the named call stack for the duration of the run. Enabled here
    // (the env is built, so any preamble calls during setup stayed
    // untracked); a normal `culebra <file>` run never sets this.
    interp_gc().dap_set_tracking(true);
    Value val;
    int code = 0;
    try {
      if (!interpret_modules(modules, env, val, msgs, dbg)) {
        for (const auto& m : msgs) diag(m + "\n");
        code = 1;
      }
    } catch (const CulebraError& e) {
      diag(format_error_message(e) + "\n");
      code = 1;
    } catch (...) {
      code = 1;
    }
    interp_gc().dap_set_tracking(false);
    return code;
  }

  std::vector<DebugFrame> frames() override {
    std::vector<DebugFrame> out;
    for (const auto& f : build_frames())
      out.push_back({f.name, f.path, f.line});
    return out;
  }

  std::vector<DebugVar> variables(size_t frame_ix) override {
    std::vector<DebugVar> out;
    auto frames = build_frames();
    if (frame_ix >= frames.size()) return out;
    Environment* env = frames[frame_ix].env;
    const peg::Ast* ast = frames[frame_ix].ast;
    if (!env || !ast) return out;
    // Builtin globals (IO, the exception types, namespaces, ...) are in scope
    // everywhere; listing them as "locals" is just noise, so skip them.
    static const std::set<std::string, std::less<>> builtins =
        builtin_global_names();
    std::set<std::string> names;
    enum_idents(*scope_node(*ast), names);
    // Plus the module top level's own names, which are in scope everywhere the
    // frame does not shadow them. Without this a lambda frame lists only its
    // locals while a named function's frame lists the globals too, because
    // scope_node finds a FUNCTION ancestor for one and not the other — an
    // accident of AST shape. The bytecode engine answers "this frame's live
    // bindings plus the module's", and so does this.
    enum_idents(*root_node(*ast), names);
    for (const auto& n : names) {
      if (n.empty() || n[0] == '_') continue;   // injected (__LINE__ etc.)
      if (builtins.count(n)) continue;          // builtin global, not local
      if (!env->has(n)) continue;               // not yet bound / unknown
      const Value& val = env->get(n);
      if (val.type == Value::Function) continue;  // hide fns / builtins
      out.push_back({n, val.str(), val.type_name()});
    }
    return out;
  }

  bool has_name(size_t frame_ix, const std::string& name) override {
    auto frame = frame_env(frame_ix);
    return frame && frame->has(name);
  }

  bool evaluate(size_t frame_ix, const std::string& expr, DebugVar& out,
                std::string& err) override {
    auto frame = frame_env(frame_ix);
    if (!frame) {
      err = "no such frame";
      return false;
    }
    Value v;
    if (!eval_in_env(frame, expr, v, err)) return false;
    out = {"", v.str(), v.type_name()};
    return true;
  }

  bool set_variable(size_t frame_ix, const std::string& name,
                    const std::string& expr, DebugVar& out,
                    std::string& err) override {
    auto frame = frame_env(frame_ix);
    if (!frame) {
      err = "no such frame";
      return false;
    }
    Value v;
    if (!eval_in_env(frame, expr, v, err)) return false;
    // assign() walks the outer chain and asserts the name exists — that
    // assert is compiled out in release, so an unknown name (a non-conformant
    // client) would deref a null root. Guard with the same chained lookup.
    if (!frame->has(name)) {
      err = "no variable '" + name + "' in scope";
      return false;
    }
    try {
      frame->assign(name, v);  // honours immutability (throws if `let`-bound)
    } catch (const CulebraError& e) {
      err = std::string(e.kind) + ": " + e.what();
      return false;
    } catch (const std::exception& e) {
      err = e.what();
      return false;
    }
    out = {name, v.str(), v.type_name()};
    return true;
  }

 private:
  // One reconstructed call-stack frame: its name, current execution point
  // (file:line), the env to read its variables from, and the AST node whose
  // enclosing function bounds the in-scope identifiers.
  struct UiFrame {
    std::string name;
    int64_t line;
    std::string path;
    Environment* env;
    const peg::Ast* ast;
  };

  // Reconstruct the call stack (top frame first) from the debuggee's
  // dap_frames(). Internal delegations (a multifn dispatcher calling the
  // picked overload, marked by a null call_ast) are collapsed into the
  // user-visible call so the stack reads as written.
  std::vector<UiFrame> build_frames() {
    std::vector<UiFrame> out;
    if (!cur_ast_ || !cur_env_) return out;
    int64_t cur_line = static_cast<int64_t>(cur_ast_->line);
    const std::string& cur_path = cur_ast_->path;
    // Group raw call entries (bottom -> top) into user frames, folding each
    // internal delegation's function frame into the real call it implements.
    struct UF {
      std::string name;
      int64_t line;
      std::string path;
      const peg::Ast* call_ast;
      Environment* exec;    // the function frame actually executing
      Environment* caller;  // the scope that made the call
    };
    std::vector<UF> ufs;
    for (const auto& e : interp_gc().dap_frames()) {
      if (e.call_ast != nullptr || ufs.empty()) {
        ufs.push_back({e.name, e.line, e.path, e.call_ast, e.callee_env,
                       e.caller_env});
      } else if (e.callee_env) {
        ufs.back().exec = e.callee_env;  // dispatcher -> picked overload
      }
    }
    int64_t m = static_cast<int64_t>(ufs.size());
    // Top: the current execution point in the innermost function.
    out.push_back({m ? ufs[m - 1].name : std::string("main"), cur_line,
                   cur_path, cur_env_, cur_ast_});
    // Each lower frame executes at the call site of the frame just above it.
    for (int64_t i = m - 1; i >= 1; i--) {
      out.push_back({ufs[i - 1].name, ufs[i].line,
                     ufs[i].path.empty() ? cur_path : ufs[i].path,
                     ufs[i - 1].exec, ufs[i].call_ast});
    }
    if (m >= 1) {
      out.push_back({"main", ufs[0].line,
                     ufs[0].path.empty() ? cur_path : ufs[0].path,
                     ufs[0].caller, ufs[0].call_ast});
    }
    return out;
  }

  // The env of the requested frame, as a shared_ptr safe to hold across the
  // eval that follows.
  std::shared_ptr<Environment> frame_env(size_t frame_ix) {
    auto frames = build_frames();
    if (frame_ix >= frames.size() || !frames[frame_ix].env) return nullptr;
    return frames[frame_ix].env->shared_from_this();
  }

  // Parse and evaluate an expression string against `frame` on a fresh
  // Interpreter. Uses the bare `eval` (not `interpret`, which would flush the
  // frame's pending defers). The fresh Interpreter carries no debugger, so
  // evaluating a call won't re-enter the hook.
  bool eval_in_env(const std::shared_ptr<Environment>& frame,
                   const std::string& expr, Value& out, std::string& err) {
    std::vector<std::string> msgs;
    auto ast = parse_with_transforms("(dap)", expr, msgs);
    if (!ast) {
      err = msgs.empty() ? "parse error" : msgs.front();
      return false;
    }
    struct Suppress {
      bool& flag;
      Suppress(bool& f) : flag(f) { flag = true; }
      ~Suppress() { flag = false; }
    } suppress{evaluating_};
    try {
      out = std::make_shared<Interpreter>()->eval(*ast, frame);
      return true;
    } catch (const CulebraError& e) {
      err = std::string(e.kind) + ": " + e.what();
    } catch (const std::exception& e) {
      err = e.what();
    }
    return false;
  }

  // Identifiers referenced in `node`, not descending into nested functions
  // (their locals belong to a different scope). Mirrors the CLI debugger so
  // the pane shows names in scope rather than every stdlib binding.
  static void enum_idents(const peg::Ast& node, std::set<std::string>& out) {
    using namespace peg::udl;
    for (const auto& c : node.nodes) {
      if (c->tag == "IDENTIFIER"_)
        out.insert(std::string(c->token));
      else if (c->tag != "FUNCTION"_)
        enum_idents(*c, out);
    }
  }

  // The enclosing function subtree, or the whole program when at top level.
  static const peg::Ast* scope_node(const peg::Ast& ast) {
    using namespace peg::udl;
    const peg::Ast* node = &ast;
    std::shared_ptr<peg::Ast> root;
    for (auto p = ast.parent.lock(); p; p = p->parent.lock()) {
      if (p->tag == "FUNCTION"_) return p.get();
      root = p;
    }
    return root ? root.get() : node;
  }

  // The whole program. enum_idents does not descend into nested functions, so
  // walking from here collects the module's top-level names and nothing else.
  static const peg::Ast* root_node(const peg::Ast& ast) {
    const peg::Ast* node = &ast;
    for (auto p = ast.parent.lock(); p; p = p->parent.lock()) node = p.get();
    return node;
  }

  const peg::Ast* cur_ast_ = nullptr;
  Environment* cur_env_ = nullptr;
  bool evaluating_ = false;
};

#ifdef CULEBRA_JIT_ENABLED

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
      const peg::Ast* stdlib = nullptr;
      if (modules.size() == 2 &&
          modules.front().abs_path == culebra::kStdlibPreamblePath) {
        stdlib = modules.front().ast.get();
      } else if (modules.size() != 1) {
        throw CulebraError("VmError", "--vm: unsupported: multi-module script");
      }
      prog_ = std::make_unique<vm::VmProgram>(vm::Compiler::compile_module(
          *modules.back().ast, stdlib, vm::Debug::Step));
      vm::Exec::run(*prog_);
    } catch (const CulebraError& e) {
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

#endif  // CULEBRA_JIT_ENABLED

// Which engine `culebra dap` debugs on.
enum class DebugEngineKind { Interp, Vm };

inline std::unique_ptr<DebugEngine> make_debug_engine(DebugEngineKind kind) {
#ifdef CULEBRA_JIT_ENABLED
  if (kind == DebugEngineKind::Vm) return std::make_unique<VmDebugEngine>();
#else
  (void)kind;
#endif
  return std::make_unique<InterpDebugEngine>();
}

}  // namespace culebra
