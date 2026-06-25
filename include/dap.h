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
// MVP scope (read-only inspection): breakpoints, single top stack frame,
// variables (current scope chain), stepping, continue. Deferred: setVariable,
// evaluate/watch, conditional breakpoints, multi-frame named stack.

#include <climits>
#include <cstdlib>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
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
  static Value L(long n) { return Value(n); }
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
    ssize_t n = ::read(in_, buf, sizeof(buf));
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
    ssize_t off = 0, total = static_cast<ssize_t>(frame.size());
    while (off < total) {
      ssize_t n = ::write(out_, frame.data() + off, total - off);
      if (n <= 0) break;
      off += n;
    }
  }
  void respond(const Value& req, Value body, bool success = true) {
    send(Obj()
             .set("type", S("response"))
             .set("seq", L(seq_++))
             .set("request_seq", at(req, "seq").type == Value::Long
                                     ? L(at(req, "seq").to_long())
                                     : L(0))
             .set("success", Bv(success))
             .set("command", S(std::string(at(req, "command").type ==
                                                   Value::String
                                               ? at(req, "command").to_string()
                                               : "")))
             .set("body", std::move(body))
             .v());
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
      respond(req, Obj()
                       .set("scopes",
                            arr({Obj()
                                     .set("name", S("Locals"))
                                     .set("variablesReference", L(1))
                                     .set("expensive", Bv(false))
                                     .v()}))
                       .v());
    } else if (cmd == "variables") {
      respond(req, Obj().set("variables", collect_variables()).v());
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
    char buf[PATH_MAX];
    std::string r = ::realpath(p.c_str(), buf) ? std::string(buf) : p;
    return canon_cache_.emplace(p, std::move(r)).first->second;
  }

  void do_set_breakpoints(const Value& req) {
    const Value& a = at(req, "arguments");
    std::string path = at(at(a, "source"), "path").type == Value::String
                           ? std::string(at(at(a, "source"), "path").to_string())
                           : "";
    std::set<long> lines;
    std::vector<Value> verified;
    const Value& bpArr = at(a, "breakpoints");
    if (bpArr.type == Value::Array) {
      for (const auto& bp : *bpArr.to_array().values) {
        long line = at(bp, "line").type == Value::Long ? at(bp, "line").to_long()
                                                       : 0;
        lines.insert(line);
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

  void do_stack_trace(const Value& req) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Value> frames;
    if (paused_ && cur_ast_) {
      std::string name = frame_name(*cur_ast_);
      std::string base = cur_path_;
      auto slash = base.find_last_of('/');
      if (slash != std::string::npos) base = base.substr(slash + 1);
      frames.push_back(
          Obj()
              .set("id", L(1))
              .set("name", S(std::move(name)))
              .set("line", L(cur_line_))
              .set("column", L(1))
              .set("source", Obj()
                                 .set("name", S(std::move(base)))
                                 .set("path", S(cur_path_))
                                 .v())
              .v());
    }
    long total = static_cast<long>(frames.size());
    respond(req, Obj()
                     .set("stackFrames", arr(std::move(frames)))
                     .set("totalFrames", L(total))
                     .v());
  }

  static std::string frame_name(const peg::Ast& ast) {
    using namespace peg::udl;
    auto node = ast.parent.lock();
    while (node) {
      if (node->tag == "FUNCTION"_) {
        // A named `fn name(...)` carries its IDENTIFIER as a child token.
        for (const auto& c : node->nodes)
          if (c->tag == "IDENTIFIER"_) return std::string(c->token);
        return "<anonymous>";
      }
      node = node->parent.lock();
    }
    return "main";
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

  Value collect_variables() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!paused_ || !cur_env_ || !cur_ast_) return arr({});
    std::set<std::string> names;
    enum_idents(*scope_node(*cur_ast_), names);
    std::vector<Value> vars;
    for (const auto& n : names) {
      if (n.empty() || n[0] == '_') continue;       // injected (__LINE__ etc.)
      if (!cur_env_->has(n)) continue;               // not yet bound / unknown
      const Value& val = cur_env_->get(n);
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

  // ---- debuggee lifecycle ----------------------------------------------
  void maybe_start() {
    if (started_ || !launched_ || !configured_) return;
    started_ = true;
    debuggee_ = std::thread([this] { debuggee_main(); });
  }

  void debuggee_main() {
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
    auto env = culebra::environment(argv_);
    // CLI-style global aliases so a script using bare puts/print behaves like
    // a normal `culebra <file>` run.
    const auto& io = env->get("IO").to_object();
    env->initialize("puts", io.get("puts"), false);
    env->initialize("print", io.get("print"), false);

    Debugger dbg = [this](const peg::Ast& a, Environment& e, bool f) {
      on_statement(a, e, f);
    };
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
    finish(code);
  }

  // Per-statement hook (runs on the debuggee thread).
  void on_statement(const peg::Ast& ast, Environment& env, bool force) {
    long line = static_cast<long>(ast.line);
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
      std::lock_guard<std::mutex> lk(bp_mu_);
      auto it = bps_.find(canon(path));
      if (it != bps_.end() && it->second.count(line)) hit = true;
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
    if (capturing_.exchange(false)) {
      ::close(1);
      ::close(2);
      if (out_reader_.joinable()) out_reader_.join();
    }
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
  void setup_output_capture() {
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
  }
  void teardown_output_capture() {
    // Normally finish() already stopped capture; this covers the path where no
    // debuggee ran (capturing_ still true).
    stop_capture();
    if (out_pipe_r_ >= 0) {
      ::close(out_pipe_r_);
      out_pipe_r_ = -1;
    }
  }

  int in_, out_;
  std::vector<std::string> argv_;
  std::string inbuf_;
  std::atomic<long> seq_{1};
  std::mutex write_mu_;

  std::thread debuggee_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool paused_ = false;
  Step step_ = Step::RUN;
  size_t step_level_ = 0;
  const peg::Ast* cur_ast_ = nullptr;
  Environment* cur_env_ = nullptr;
  std::string cur_path_;
  long cur_line_ = 0;
  bool entered_ = false;

  std::map<std::string, std::set<long>> bps_;
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
