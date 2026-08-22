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
// and watch / hover / debug-console evaluation.
//
// Nothing here knows which engine runs the program: the frames, the names in
// scope and the meaning of an evaluated expression all come from a
// DebugEngine (include/debug_engine.h). Those queries run on the debuggee's
// own thread while it is parked — the frame state belongs to it — so this
// file hands them over as jobs rather than reaching across (see run_parked).

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
#include <debug_engine.h>
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
  // No default engine: which one a debug session runs on is the caller's to
  // say, the way make_debug_engine / make_test_host take it as a parameter.
  DapServer(int in_fd, int out_fd, std::vector<std::string> argv,
            DebugEngineKind engine)
      : in_(in_fd),
        out_(out_fd),
        argv_(std::move(argv)),
        engine_(make_debug_engine(engine)) {}

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

  // Runs `fn` on the parked debuggee thread and waits for it. Everything a
  // client asks about a stopped program — frames, variables, evaluate — is a
  // question about state that belongs to that thread, so this is the only way
  // the DAP loop is allowed to ask. Returns false when nothing is parked,
  // which is the "not paused" every such request answers with.
  bool run_parked(const std::function<void()>& fn) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!paused_) return false;
    job_ = &fn;
    job_done_ = false;
    cv_.notify_all();
    job_done_cv_.wait(lk, [this] { return job_done_; });
    return true;
  }

  void do_stack_trace(const Value& req) {
    std::vector<DebugFrame> frames;
    run_parked([&] { frames = engine_->frames(); });
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

  // Variables in scope at a given frame. `var_ref` is the DAP
  // variablesReference, which we set equal to the 1-based frame id (see
  // `scopes`), so a client inspecting a selected frame gets that frame's
  // locals.
  Value collect_variables(int64_t var_ref) {
    std::vector<DebugVar> vars;
    run_parked([&] { vars = engine_->variables(frame_index(var_ref)); });
    std::vector<Value> out;
    for (auto& v : vars)
      out.push_back(Obj()
                        .set("name", S(v.name))
                        .set("value", S(v.value))
                        .set("type", S(v.type))
                        .set("variablesReference", L(0))
                        .v());
    return arr(std::move(out));
  }

  // A frame id arg (`frameId`, or `variablesReference` which we set equal to
  // the frame id) clamped to a 1-based frame index; 0 / absent / negative
  // means the top frame.
  static int64_t frame_id_arg(const Value& a, const char* key = "frameId") {
    const Value& f = at(a, key);
    int64_t id = f.type == Value::Long ? f.to_long() : 0;
    return id >= 1 ? id : 1;
  }
  static size_t frame_index(int64_t frame_id) {
    return static_cast<size_t>(frame_id >= 1 ? frame_id - 1 : 0);
  }

  void do_evaluate(const Value& req) {
    const Value& a = at(req, "arguments");
    std::string expr = at(a, "expression").type == Value::String
                           ? std::string(at(a, "expression").to_string())
                           : "";
    size_t ix = frame_index(frame_id_arg(a));
    std::string err = "not paused";
    DebugVar v;
    bool ok = false;
    run_parked([&] { ok = engine_->evaluate(ix, expr, v, err); });
    if (ok) {
      respond(req, Obj()
                       .set("result", S(v.value))
                       .set("type", S(v.type))
                       .set("variablesReference", L(0))
                       .v());
    } else {
      respond(req,
              Obj().set("result", S(err)).set("variablesReference", L(0)).v(),
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
    size_t ix = frame_index(frame_id_arg(a, "variablesReference"));
    std::string err = "not paused";
    DebugVar v;
    bool ok = false;
    run_parked([&] {
      // A name the frame does not have is the client's mistake, not the
      // engine's: report it before an assignment invents a binding.
      if (!engine_->has_name(ix, name)) {
        err = "no variable '" + name + "' in scope";
        return;
      }
      ok = engine_->set_variable(ix, name, valexpr, v, err);
    });
    if (!ok) {
      respond(req, Obj().v(), /*success=*/false, err);
      return;
    }
    respond(req, Obj()
                     .set("value", S(v.value))
                     .set("type", S(v.type))
                     .set("variablesReference", L(0))
                     .v());
  }

  // Evaluate a breakpoint condition against the live frame (we are on the
  // debuggee thread at a statement safe point, so the engine answers
  // directly). Stop when it is truthy; also stop — and report once — when it
  // can't be evaluated or isn't boolean, so a bad condition surfaces instead
  // of silently disabling the breakpoint.
  bool eval_breakpoint_condition(const std::string& cond) {
    DebugVar v;
    std::string err;
    if (!engine_->evaluate(0, cond, v, err)) {
      emit_stderr("breakpoint condition error: " + err + "\n");
      return true;
    }
    if (v.value == "true") return true;
    if (v.value == "false") return false;
    emit_stderr("breakpoint condition is not a boolean: " + cond + "\n");
    return true;
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
    int code = engine_->run(
        program_, argv_,
        [this](const std::string& path, int64_t line, bool force,
               size_t depth) { on_statement(path, line, force, depth); },
        [this](const std::string& text) { emit_stderr(text); });
    finish(code);
  }

  // Per-statement hook (runs on the debuggee thread).
  void on_statement(const std::string& path, int64_t line, bool force,
                    size_t depth) {
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
      // Evaluate any condition outside bp_mu_ (it runs the program).
      if (matched) hit = cond.empty() || eval_breakpoint_condition(cond);
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
      else if (m == Step::NEXT && depth <= lvl)
        hit = true, reason = "step";
      else if (m == Step::STEP_OUT && depth < lvl)
        hit = true, reason = "step";
    }
    entered_ = true;
    if (!hit) return;

    {
      std::unique_lock<std::mutex> lk(mu_);
      cur_depth_ = depth;
      paused_ = true;
    }
    event("stopped", Obj()
                         .set("reason", S(reason))
                         .set("threadId", L(1))
                         .set("allThreadsStopped", Bv(true))
                         .v());
    // Park, serving the DAP loop's queries in between: they read frames and
    // registers that belong to this thread, so this thread is what runs them.
    std::unique_lock<std::mutex> lk(mu_);
    while (paused_) {
      cv_.wait(lk, [this] { return !paused_ || job_; });
      if (!job_) continue;
      const auto* job = job_;
      lk.unlock();
      (*job)();
      lk.lock();
      job_ = nullptr;
      job_done_ = true;
      job_done_cv_.notify_all();
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
    step_level_ = cur_depth_;
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

  std::unique_ptr<DebugEngine> engine_;
  culebra::SizedThread debuggee_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool paused_ = false;
  Step step_ = Step::RUN;
  size_t step_level_ = 0;
  size_t cur_depth_ = 0;  // the parked frame's depth, for step over / out
  bool entered_ = false;
  // A query for the parked debuggee thread to run, and the handshake back.
  // One at a time: the DAP loop is what posts them, and it is sequential.
  const std::function<void()>* job_ = nullptr;
  bool job_done_ = false;
  std::condition_variable job_done_cv_;

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
