#pragma once

// Debug Adapter Protocol (DAP) server for the interpreter — `culebra dap`.
//
// VSCode / Neovim (nvim-dap) / Vim (vimspector) / Zed and any other DAP client
// drive this over stdio (Content-Length-framed JSON). It runs the script on a
// separate thread with the interpreter's per-statement debug hook; on a line
// breakpoint / `debugger` statement / step it pauses on a condition variable,
// while the DAP loop on the main thread answers stackTrace / scopes /
// variables and resumes on continue / next / stepIn / stepOut. The debuggee's
// stdout/stderr is captured (fd redirect) and forwarded as `output` events so
// it never corrupts the DAP stream.
//
// Supported: line + conditional breakpoints, a named multi-frame call stack
// (with per-frame variables, evaluate, and setVariable), stepping, continue,
// and watch / hover / debug-console evaluation. interp only — the tree-walker
// exposes the AST + Environment a source-level debugger needs.

#include <climits>
#include <cstdlib>
#if defined(_WIN32)
#include <io.h>  // _read / _write (DAP stdio transport)
#else
#include <unistd.h>
#endif

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <culebra.h>
#include <module_loader.h>
#include <stdlib_interp.h>

namespace culebra {

// Byte-stream read/write on a raw fd for the DAP stdio transport. On Windows the
// POSIX ::read/::write live in <io.h> under an underscore; the fd values (0/1)
// are the same CRT descriptors.
namespace _dap_io {
inline int64_t read_fd(int fd, char* buf, size_t n) {
#if defined(_WIN32)
  return _read(fd, buf, static_cast<unsigned>(n));
#else
  return ::read(fd, buf, n);
#endif
}
inline int64_t write_fd(int fd, const char* buf, size_t n) {
#if defined(_WIN32)
  return _write(fd, buf, static_cast<unsigned>(n));
#else
  return ::write(fd, buf, n);
#endif
}
}  // namespace _dap_io

class DapServer {
 public:
  DapServer(int in_fd, int out_fd, std::vector<std::string> argv)
      : in_(in_fd), out_(out_fd), argv_(std::move(argv)) {}

  int run() {
    setup_output_capture();
    std::string body;
    while (!should_exit_ && read_message(body)) {
      try {
        handle(json_parse(body, "auto"));
      } catch (const std::exception&) {
        // A malformed message shouldn't kill the session.
      }
    }
    // Make sure a paused debuggee is released so its thread can exit.
    request_resume(Step::RUN);
    if (debuggee_.joinable()) debuggee_.join();
    teardown_output_capture();
    return 0;
  }

 private:
  enum class Step { RUN, NEXT, STEP_IN, STEP_OUT };

  // ---- value builders (DAP JSON is built as culebra Values, serialized by
  // the interpreter's json_stringify) -------------------------------------
  static Value S(std::string s) { return Value(std::move(s)); }
  static Value L(int64_t n) { return Value(n); }
  static Value Bv(bool b) { return Value(b); }
  struct Obj {
    ObjectValue o;
    Obj& set(std::string_view k, Value v) {
      o.initialize(k, v, false);
      return *this;
    }
    Value v() { return Value(std::move(o)); }
  };
  static Value arr(std::vector<Value> xs) {
    ArrayValue a;
    for (auto& x : xs) a.values->push_back(std::move(x));
    return Value(std::move(a));
  }
  static const Value& at(const Value& v, const char* k) {
    static const Value kNil;
    if (v.type != Value::Object) return kNil;
    const auto& o = v.to_object();
    return o.has(k) ? o.get(k) : kNil;
  }

  // ---- transport --------------------------------------------------------
  bool fill() {
    char buf[4096];
    int64_t n = _dap_io::read_fd(in_, buf, sizeof(buf));
    if (n <= 0) return false;
    inbuf_.append(buf, static_cast<size_t>(n));
    return true;
  }
  bool read_message(std::string& body) {
    size_t hdr_end;
    while ((hdr_end = inbuf_.find("\r\n\r\n")) == std::string::npos) {
      if (!fill()) return false;
    }
    std::string headers = inbuf_.substr(0, hdr_end);
    size_t len = 0;
    auto p = headers.find("Content-Length:");
    if (p != std::string::npos)
      len = std::strtoul(headers.c_str() + p + 15, nullptr, 10);
    size_t start = hdr_end + 4;
    while (inbuf_.size() < start + len) {
      if (!fill()) return false;
    }
    body = inbuf_.substr(start, len);
    inbuf_.erase(0, start + len);
    return true;
  }
  void send(const Value& msg) {
    std::string body = json_stringify(msg, 0, false);
    std::string frame =
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::lock_guard<std::mutex> lk(write_mu_);
    int64_t off = 0, total = static_cast<long>(frame.size());
    while (off < total) {
      int64_t n = _dap_io::write_fd(out_, frame.data() + off, total - off);
      if (n <= 0) break;
      off += n;
    }
  }
  void respond(const Value& req, Value body, bool success = true,
               const std::string& message = "") {
    Obj r;
    r.set("type", S("response"))
        .set("seq", L(seq_++))
        .set("request_seq", at(req, "seq").type == Value::Long
                                ? L(at(req, "seq").to_long())
                                : L(0))
        .set("success", Bv(success))
        .set("command", S(std::string(at(req, "command").type == Value::String
                                          ? at(req, "command").to_string()
                                          : "")));
    if (!message.empty()) r.set("message", S(message));  // shown on failures
    r.set("body", std::move(body));
    send(r.v());
  }
  void event(const char* name, Value body) {
    send(Obj()
             .set("type", S("event"))
             .set("seq", L(seq_++))
             .set("event", S(name))
             .set("body", std::move(body))
             .v());
  }

  // ---- request dispatch -------------------------------------------------
  void handle(const Value& req) {
    std::string cmd =
        at(req, "command").type == Value::String
            ? std::string(at(req, "command").to_string())
            : "";
    if (cmd == "initialize") {
      respond(req, Obj()
                       .set("supportsConfigurationDoneRequest", Bv(true))
                       .set("supportsSetVariable", Bv(true))
                       .set("supportsEvaluateForHovers", Bv(true))
                       .set("supportsConditionalBreakpoints", Bv(true))
                       .v());
      event("initialized", Obj().v());
    } else if (cmd == "launch") {
      const Value& a = at(req, "arguments");
      if (at(a, "program").type == Value::String)
        program_ = std::string(at(a, "program").to_string());
      stop_on_entry_ = at(a, "stopOnEntry").type == Value::Bool &&
                       at(a, "stopOnEntry").to_bool();
      launched_ = true;
      respond(req, Obj().v());
      maybe_start();
    } else if (cmd == "setBreakpoints") {
      do_set_breakpoints(req);
    } else if (cmd == "setExceptionBreakpoints") {
      respond(req, Obj().set("breakpoints", arr({})).v());
    } else if (cmd == "configurationDone") {
      configured_ = true;
      respond(req, Obj().v());
      maybe_start();
    } else if (cmd == "threads") {
      respond(req, Obj()
                       .set("threads",
                            arr({Obj().set("id", L(1)).set("name", S("main")).v()}))
                       .v());
    } else if (cmd == "stackTrace") {
      do_stack_trace(req);
    } else if (cmd == "scopes") {
      // variablesReference == the frame id, so `variables` reads the locals of
      // whichever frame the client selected in the call stack.
      int64_t frame_id = frame_id_arg(at(req, "arguments"));
      respond(req, Obj()
                       .set("scopes",
                            arr({Obj()
                                     .set("name", S("Locals"))
                                     .set("variablesReference", L(frame_id))
                                     .set("expensive", Bv(false))
                                     .v()}))
                       .v());
    } else if (cmd == "variables") {
      int64_t var_ref = frame_id_arg(at(req, "arguments"), "variablesReference");
      respond(req, Obj().set("variables", collect_variables(var_ref)).v());
    } else if (cmd == "evaluate") {
      do_evaluate(req);
    } else if (cmd == "setVariable") {
      do_set_variable(req);
    } else if (cmd == "continue") {
      request_resume(Step::RUN);
      respond(req, Obj().set("allThreadsContinued", Bv(true)).v());
    } else if (cmd == "next") {
      request_resume(Step::NEXT);
      respond(req, Obj().v());
    } else if (cmd == "stepIn") {
      request_resume(Step::STEP_IN);
      respond(req, Obj().v());
    } else if (cmd == "stepOut") {
      request_resume(Step::STEP_OUT);
      respond(req, Obj().v());
    } else if (cmd == "disconnect" || cmd == "terminate") {
      {
        std::lock_guard<std::mutex> lk(bp_mu_);
        bps_.clear();
        bp_count_.store(0, std::memory_order_relaxed);
      }
      request_resume(Step::RUN);
      respond(req, Obj().v());
      should_exit_ = true;
    } else {
      // Unknown / unsupported request: a benign failure keeps the session up.
      respond(req, Obj().v(), /*success=*/false);
    }
  }

  // Canonical (symlink-resolved) path, cached. Called only under bp_mu_. The
  // module loader canonicalizes AST paths (e.g. macOS /var -> /private/var), so
  // breakpoints must match on the canonical path, not the raw launch path.
  const std::string& canon(const std::string& p) {
    auto it = canon_cache_.find(p);
    if (it != canon_cache_.end()) return it->second;
#if defined(_WIN32)
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(p, ec);
    std::string r = ec ? p : c.string();
#else
    char buf[PATH_MAX];
    std::string r = ::realpath(p.c_str(), buf) ? std::string(buf) : p;
#endif
    return canon_cache_.emplace(p, std::move(r)).first->second;
  }

  void do_set_breakpoints(const Value& req) {
    const Value& a = at(req, "arguments");
    std::string path = at(at(a, "source"), "path").type == Value::String
                           ? std::string(at(at(a, "source"), "path").to_string())
                           : "";
    std::map<long, std::string> lines;
    std::vector<Value> verified;
    const Value& bpArr = at(a, "breakpoints");
    if (bpArr.type == Value::Array) {
      for (const auto& bp : *bpArr.to_array().values) {
        int64_t line = at(bp, "line").type == Value::Long ? at(bp, "line").to_long()
                                                       : 0;
        std::string cond = at(bp, "condition").type == Value::String
                               ? std::string(at(bp, "condition").to_string())
                               : "";
        lines[line] = std::move(cond);
        verified.push_back(
            Obj().set("verified", Bv(true)).set("line", L(line)).v());
      }
    }
    {
      std::lock_guard<std::mutex> lk(bp_mu_);
      bps_[canon(path)] = std::move(lines);
      size_t n = 0;
      for (const auto& e : bps_) n += e.second.size();
      bp_count_.store(n, std::memory_order_relaxed);
    }
    respond(req, Obj().set("breakpoints", arr(std::move(verified))).v());
  }

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

  // The debuggee thread's GC substate (holds dap_frames_). Null until the
  // debuggee starts. Safe to read from the DAP thread only while paused.
  InterpGC* debuggee_gc() const {
    if (!debuggee_rt_) return nullptr;
    return static_cast<InterpGC*>(debuggee_rt_->substate[kSlotInterpGc]);
  }

  // Reconstruct the call stack (top frame first) from the debuggee's
  // dap_frames(). Internal delegations (a multifn dispatcher calling the picked
  // overload, marked by a null call_ast) are collapsed into the user-visible
  // call so the stack reads as written. Must be called under mu_ while paused.
  std::vector<UiFrame> build_frames_locked() {
    std::vector<UiFrame> out;
    if (!paused_ || !cur_ast_ || !cur_env_) return out;
    InterpGC* gc = debuggee_gc();
    if (!gc) {
      out.push_back({"main", cur_line_, cur_path_, cur_env_, cur_ast_});
      return out;
    }
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
    for (const auto& e : gc->dap_frames()) {
      if (e.call_ast != nullptr || ufs.empty()) {
        ufs.push_back({e.name, e.line, e.path, e.call_ast, e.callee_env,
                       e.caller_env});
      } else if (e.callee_env) {
        ufs.back().exec = e.callee_env;  // dispatcher -> picked overload
      }
    }
    int64_t m = static_cast<int64_t>(ufs.size());
    // Top: the current execution point in the innermost function.
    out.push_back({m ? ufs[m - 1].name : std::string("main"), cur_line_,
                   cur_path_, cur_env_, cur_ast_});
    // Each lower frame executes at the call site of the frame just above it.
    for (int64_t i = m - 1; i >= 1; i--) {
      out.push_back({ufs[i - 1].name, ufs[i].line,
                     ufs[i].path.empty() ? cur_path_ : ufs[i].path,
                     ufs[i - 1].exec, ufs[i].call_ast});
    }
    if (m >= 1) {
      out.push_back({"main", ufs[0].line,
                     ufs[0].path.empty() ? cur_path_ : ufs[0].path,
                     ufs[0].caller, ufs[0].call_ast});
    }
    return out;
  }

  // Resolve a 1-based DAP frame id to its env + scope AST (under mu_).
  bool frame_target_locked(int64_t frame_id, Environment*& env,
                           const peg::Ast*& ast) {
    auto frames = build_frames_locked();
    if (frame_id < 1 || static_cast<size_t>(frame_id) > frames.size())
      return false;
    env = frames[frame_id - 1].env;
    ast = frames[frame_id - 1].ast;
    return true;
  }

  void do_stack_trace(const Value& req) {
    std::lock_guard<std::mutex> lk(mu_);
    auto frames = build_frames_locked();
    std::vector<Value> out;
    int64_t id = 1;
    for (auto& fr : frames) {
      std::string base = fr.path;
      auto slash = base.find_last_of('/');
      if (slash != std::string::npos) base = base.substr(slash + 1);
      out.push_back(Obj()
                        .set("id", L(id++))
                        .set("name", S(std::move(fr.name)))
                        .set("line", L(fr.line))
                        .set("column", L(1))
                        .set("source", Obj()
                                           .set("name", S(std::move(base)))
                                           .set("path", S(fr.path))
                                           .v())
                        .v());
    }
    int64_t total = static_cast<long>(out.size());
    respond(req, Obj()
                     .set("stackFrames", arr(std::move(out)))
                     .set("totalFrames", L(total))
                     .v());
  }

  // Identifiers referenced in `node`, not descending into nested functions
  // (their locals belong to a different scope). Mirrors the CLI debugger so the
  // pane shows names in scope rather than every stdlib binding in the root env.
  static void enum_idents(const peg::Ast& node,
                          std::set<std::string>& out) {
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

  // Variables in scope at a given frame. `var_ref` is the DAP
  // variablesReference, which we set equal to the 1-based frame id (see
  // `scopes`), so a client inspecting a selected frame gets that frame's locals.
  Value collect_variables(int64_t var_ref) {
    std::lock_guard<std::mutex> lk(mu_);
    Environment* env = nullptr;
    const peg::Ast* ast = nullptr;
    if (!frame_target_locked(var_ref, env, ast) || !env || !ast) return arr({});
    // Builtin globals (IO, the exception types, namespaces, ...) are in scope
    // everywhere; listing them as "locals" is just noise, so skip them.
    static const std::set<std::string, std::less<>> builtins =
        builtin_global_names();
    std::set<std::string> names;
    enum_idents(*scope_node(*ast), names);
    std::vector<Value> vars;
    for (const auto& n : names) {
      if (n.empty() || n[0] == '_') continue;       // injected (__LINE__ etc.)
      if (builtins.count(n)) continue;               // builtin global, not local
      if (!env->has(n)) continue;                    // not yet bound / unknown
      const Value& val = env->get(n);
      if (val.type == Value::Function) continue;     // hide fns / builtins
      vars.push_back(Obj()
                         .set("name", S(n))
                         .set("value", S(val.str()))
                         .set("type", S(val.type_name()))
                         .set("variablesReference", L(0))
                         .v());
    }
    return arr(std::move(vars));
  }

  // Parse and evaluate an expression string against `frame` on a fresh
  // Interpreter. The debuggee is parked, so reading its env is race-free, and
  // the frame's values stay alive through the env's own refcounted references
  // regardless of which thread's GC the eval allocates on. Uses the bare `eval`
  // (not `interpret`, which would flush the frame's pending defers). The fresh
  // Interpreter carries no debugger, so evaluating a call won't re-enter the
  // hook.
  bool eval_in_env(const std::shared_ptr<Environment>& frame,
                   const std::string& expr, Value& out, std::string& err) {
    if (!frame) { err = "no frame"; return false; }
    std::vector<std::string> msgs;
    auto ast = parse_with_transforms("(dap)", expr, msgs);
    if (!ast) { err = msgs.empty() ? "parse error" : msgs.front(); return false; }
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

  // The env of the requested frame (default: top), as a shared_ptr safe to use
  // after releasing mu_. The debuggee thread is parked, so this read is safe.
  std::shared_ptr<Environment> resolve_frame(int64_t frame_id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!paused_) { err = "not paused"; return nullptr; }
    Environment* env = nullptr;
    const peg::Ast* ast = nullptr;
    if (!frame_target_locked(frame_id, env, ast) || !env) {
      err = "no such frame";
      return nullptr;
    }
    return env->shared_from_this();
  }

  // A frame id arg (`frameId`, or `variablesReference` which we set equal to the
  // frame id) clamped to a 1-based frame index; 0 / absent / negative means the
  // top frame.
  static int64_t frame_id_arg(const Value& a, const char* key = "frameId") {
    const Value& f = at(a, key);
    int64_t id = f.type == Value::Long ? f.to_long() : 0;
    return id >= 1 ? id : 1;
  }

  void do_evaluate(const Value& req) {
    const Value& a = at(req, "arguments");
    std::string expr = at(a, "expression").type == Value::String
                           ? std::string(at(a, "expression").to_string())
                           : "";
    std::string err;
    auto frame = resolve_frame(frame_id_arg(a), err);
    Value v;
    if (frame && eval_in_env(frame, expr, v, err)) {
      respond(req, Obj()
                       .set("result", S(v.str()))
                       .set("type", S(v.type_name()))
                       .set("variablesReference", L(0))
                       .v());
    } else {
      respond(req, Obj().set("result", S(err)).set("variablesReference", L(0)).v(),
              /*success=*/false, err);
    }
  }

  void do_set_variable(const Value& req) {
    const Value& a = at(req, "arguments");
    std::string name = at(a, "name").type == Value::String
                           ? std::string(at(a, "name").to_string())
                           : "";
    std::string valexpr = at(a, "value").type == Value::String
                              ? std::string(at(a, "value").to_string())
                              : "";
    // variablesReference is the frame id (see `scopes`), so a set targets the
    // variable in the selected frame's scope, not always the top frame.
    int64_t frame_id = frame_id_arg(a, "variablesReference");
    std::string err;
    auto frame = resolve_frame(frame_id, err);
    Value v;
    if (!frame || !eval_in_env(frame, valexpr, v, err)) {
      respond(req, Obj().v(), /*success=*/false, err);
      return;
    }
    // assign() walks the outer chain and asserts the name exists — that assert
    // is compiled out in release, so an unknown name (a non-conformant client)
    // would deref a null root. Guard with the same chained lookup first.
    if (!frame->has(name)) {
      respond(req, Obj().v(), false, "no variable '" + name + "' in scope");
      return;
    }
    try {
      frame->assign(name, v);  // honours immutability (throws if `let`-bound)
    } catch (const CulebraError& e) {
      respond(req, Obj().v(), false, std::string(e.kind) + ": " + e.what());
      return;
    } catch (const std::exception& e) {
      respond(req, Obj().v(), false, e.what());
      return;
    }
    respond(req, Obj()
                     .set("value", S(v.str()))
                     .set("type", S(v.type_name()))
                     .set("variablesReference", L(0))
                     .v());
  }

  // Evaluate a breakpoint condition against the live frame (we are on the
  // debuggee thread at a statement safe point). Stop when it is truthy; also
  // stop — and report once — when it can't be evaluated or isn't boolean, so a
  // bad condition surfaces instead of silently disabling the breakpoint.
  bool eval_breakpoint_condition(const std::string& cond, Environment& env) {
    Value v;
    std::string err;
    if (!eval_in_env(env.shared_from_this(), cond, v, err)) {
      emit_stderr("breakpoint condition error: " + err + "\n");
      return true;
    }
    try {
      return v.to_bool();
    } catch (const std::exception&) {
      emit_stderr("breakpoint condition is not a boolean: " + cond + "\n");
      return true;
    }
  }

  // ---- debuggee lifecycle ----------------------------------------------
  void maybe_start() {
    if (started_ || !launched_ || !configured_) return;
    started_ = true;
    debuggee_ = culebra::SizedThread([this] { debuggee_main(); });
  }

  void debuggee_main() {
    // The debuggee's stdout is a pipe (our capture), so it is block-buffered:
    // unflushed output (e.g. `print`, which has no trailing newline) would only
    // reach the debug console at exit, looking like nothing prints until the
    // program ends. Stream it live by flushing std::cout after every write.
    // (Debug-only — a normal `culebra <file>` run keeps the buffered stdout.)
    std::cout << std::unitbuf;

    std::ifstream ifs(program_, std::ios::binary);
    std::string src((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
    if (!ifs && src.empty()) {
      event("output", Obj()
                          .set("category", S("stderr"))
                          .set("output", S("cannot open '" + program_ + "'\n"))
                          .v());
      finish(1);
      return;
    }
    std::vector<std::string> msgs;
    ModuleLoader loader;
    std::vector<LoadedModule> modules;
    try {
      modules = loader.load_program(program_, src, msgs);
    } catch (const CulebraError& e) {
      emit_error(e);
      finish(1);
      return;
    }
    culebra::sys_argv() = argv_;
    auto env = culebra::environment();
    // CLI-style global aliases so a script using bare inspect/print/println
    // behaves like a normal `culebra <file>` run.
    const auto& io = env->get("IO").to_object();
    env->initialize("inspect", io.get("inspect"), false);
    env->initialize("print", io.get("print"), false);
    env->initialize("println", io.get("println"), false);

    Debugger dbg = [this](const peg::Ast& a, Environment& e, bool f) {
      on_statement(a, e, f);
    };
    // The interpreter's runtime substate (incl. the GC + dap_frames_) is
    // thread-local, so the DAP thread can't reach it through interp_gc(). Record
    // this thread's Runtime so frame queries can read the debuggee's instance
    // while it's parked.
    debuggee_rt_ = &current_runtime();
    // Track the named call stack for the duration of the run. Enabled here (the
    // env is built, so any preamble calls during setup stayed untracked) and
    // cleared in finish(); a normal `culebra <file>` run never sets this.
    interp_gc().dap_set_tracking(true);
    Value val;
    int code = 0;
    try {
      if (!interpret_modules(modules, env, val, msgs, dbg)) {
        for (const auto& m : msgs) emit_stderr(m + "\n");
        code = 1;
      }
    } catch (const CulebraError& e) {
      emit_error(e);
      code = 1;
    } catch (...) {
      code = 1;
    }
    interp_gc().dap_set_tracking(false);
    finish(code);
  }

  // Per-statement hook (runs on the debuggee thread).
  void on_statement(const peg::Ast& ast, Environment& env, bool force) {
    int64_t line = static_cast<long>(ast.line);
    const std::string& path = ast.path;
    // The lazy stdlib preamble (Time/Args/Log/...) evaluates as statements with
    // a synthetic `<builtin>` path. Never stop there — the debugger only stops
    // in user source. (Breakpoints are file:line-keyed so they were already
    // immune; entry/step are not.)
    if (path.empty() || path == "<builtin>") return;

    bool hit = force;  // `debugger` statement
    const char* reason = "breakpoint";
    if (!hit && stop_on_entry_ && !entered_) {
      hit = true;
      reason = "entry";
    }
    if (!hit && bp_count_.load(std::memory_order_relaxed) != 0) {
      bool matched = false;
      std::string cond;
      {
        std::lock_guard<std::mutex> lk(bp_mu_);
        auto it = bps_.find(canon(path));
        if (it != bps_.end()) {
          auto lit = it->second.find(line);
          if (lit != it->second.end()) {
            matched = true;
            cond = lit->second;
          }
        }
      }
      // Evaluate any condition outside bp_mu_ (it runs the interpreter).
      if (matched) hit = cond.empty() || eval_breakpoint_condition(cond, env);
    }
    if (!hit) {
      // Snapshot the step intent under mu_: request_resume writes it from the
      // main thread (incl. the not-paused disconnect / run-exit path), so an
      // unlocked read here would be a data race.
      Step m;
      size_t lvl;
      {
        std::lock_guard<std::mutex> lk(mu_);
        m = step_;
        lvl = step_level_;
      }
      if (m == Step::STEP_IN)
        hit = true, reason = "step";
      else if (m == Step::NEXT && env.level <= lvl)
        hit = true, reason = "step";
      else if (m == Step::STEP_OUT && env.level < lvl)
        hit = true, reason = "step";
    }
    entered_ = true;
    if (!hit) return;

    {
      std::unique_lock<std::mutex> lk(mu_);
      cur_ast_ = &ast;
      cur_env_ = &env;
      cur_path_ = path;
      cur_line_ = line;
      paused_ = true;
    }
    event("stopped", Obj()
                         .set("reason", S(reason))
                         .set("threadId", L(1))
                         .set("allThreadsStopped", Bv(true))
                         .v());
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return !paused_; });
      cur_ast_ = nullptr;
      cur_env_ = nullptr;
    }
  }

  void request_resume(Step mode) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!paused_) {
      // Set the step intent even if not currently paused (harmless).
      step_ = mode;
      return;
    }
    step_ = mode;
    step_level_ = cur_env_ ? cur_env_->level : 0;
    paused_ = false;
    cv_.notify_all();
  }

  void finish(int code) {
    // Flush + close the debuggee's stdout/stderr so the capture reader drains
    // all output (then EOFs), and join it BEFORE terminating — guaranteeing
    // every `output` event precedes `terminated`.
    std::cout.flush();
    std::fflush(nullptr);
    stop_capture();
    event("exited", Obj().set("exitCode", L(code)).v());
    event("terminated", Obj().v());
  }
  // Close the debuggee's write ends (so the reader EOFs) and join it.
  // Idempotent — finish() and teardown_output_capture() both call it.
  void stop_capture() {
#if !defined(_WIN32)
    if (capturing_.exchange(false)) {
      ::close(1);
      ::close(2);
      if (out_reader_.joinable()) out_reader_.join();
    }
#endif
  }
  void emit_stderr(const std::string& s) {
    event("output", Obj().set("category", S("stderr")).set("output", S(s)).v());
  }
  void emit_error(const CulebraError& e) {
    std::string m = std::string(e.kind) + ": " + e.what();
    if (e.line > 0 || e.col > 0)
      m += " at " + std::to_string(e.line) + ":" + std::to_string(e.col) + ".";
    emit_stderr(m + "\n");
  }

  // ---- output capture (debuggee stdout/stderr -> `output` events) -------
  // Redirects the debuggee's fd 1/2 through a pipe so its output is forwarded as
  // DAP `output` events instead of corrupting the protocol stream. Not yet ported
  // to Windows (Phase 2 — _pipe/_dup2); there the debuggee writes to the real
  // stdout and DAP simply omits output events.
  void setup_output_capture() {
#if !defined(_WIN32)
    int pipefd[2];
    if (::pipe(pipefd) != 0) return;
    out_pipe_r_ = pipefd[0];
    // From here, fd 1/2 (what the debuggee writes to) go to the pipe; DAP keeps
    // writing to the original stdout via the saved `out_`.
    int saved = ::dup(out_);
    if (saved >= 0) out_ = saved;
    ::dup2(pipefd[1], 1);
    ::dup2(pipefd[1], 2);
    ::close(pipefd[1]);
    capturing_ = true;
    out_reader_ = std::thread([this] {
      char buf[4096];
      for (;;) {
        ssize_t n = ::read(out_pipe_r_, buf, sizeof(buf));
        if (n <= 0) break;
        event("output", Obj()
                            .set("category", S("stdout"))
                            .set("output", S(std::string(buf, n)))
                            .v());
      }
    });
#endif  // !_WIN32
  }
  void teardown_output_capture() {
    // Normally finish() already stopped capture; this covers the path where no
    // debuggee ran (capturing_ still true).
    stop_capture();
#if !defined(_WIN32)
    if (out_pipe_r_ >= 0) {
      ::close(out_pipe_r_);
      out_pipe_r_ = -1;
    }
#endif
  }

  int in_, out_;
  std::vector<std::string> argv_;
  std::string inbuf_;
  std::atomic<long> seq_{1};
  std::mutex write_mu_;

  culebra::SizedThread debuggee_;
  Runtime* debuggee_rt_ = nullptr;  // the debuggee thread's runtime substate
  std::mutex mu_;
  std::condition_variable cv_;
  bool paused_ = false;
  Step step_ = Step::RUN;
  size_t step_level_ = 0;
  const peg::Ast* cur_ast_ = nullptr;
  Environment* cur_env_ = nullptr;
  std::string cur_path_;
  int64_t cur_line_ = 0;
  bool entered_ = false;

  // path -> (line -> condition). An empty condition means an unconditional
  // breakpoint; a non-empty one is an expression evaluated in the stopped
  // frame's scope, and the line only stops when it is truthy.
  std::map<std::string, std::map<long, std::string>> bps_;
  std::map<std::string, std::string> canon_cache_;
  std::mutex bp_mu_;
  // Total breakpoint count, readable without bp_mu_ so the per-statement hook
  // can skip the lock + canon() lookup entirely when none are set.
  std::atomic<size_t> bp_count_{0};

  bool launched_ = false, configured_ = false, started_ = false;
  bool stop_on_entry_ = false;
  std::atomic<bool> should_exit_{false};
  std::string program_;

  std::atomic<bool> capturing_{false};
  std::thread out_reader_;
  int out_pipe_r_ = -1;
};

}  // namespace culebra
