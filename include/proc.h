#pragma once

// Type-neutral POSIX process execution core for the Proc namespace.
//
// No dependency on culebra Value / JitValue / GC: both the interp and JIT
// backends call these and adapt RunOutcome into their own object
// representation. Keeping this header value-neutral lets the binding layer and
// stdlib_jit.h include it without pulling each other in.
//
//   run_command — one command, blocking. Captures stdout/stderr without
//                 deadlocking (poll-multiplexed drain), optionally feeds stdin,
//                 reports signal death distinctly from a normal exit code.
//   run_all     — N commands, <= limit concurrent, allSettled, input order.
//   run_race    — N commands, first to finish wins; the rest are killed.
//
// run_command / run_all / run_race share one spawn + drain + reap core
// (the drain loop is the most deadlock-prone code, so it lives once).
//
// Cooperative interrupts: the blocking poll loops wake at least every
// kInterruptPollMs to honor a pending Ctrl+C (process SIGINT) or this isolate's
// cancel, SIGKILL their children, and raise the cooperative `Interrupted` via
// throw_if_interrupted() — so a single Ctrl+C stops `Proc.run` symmetrically
// across interp/JIT/AOT rather than relying on a post-call safepoint (which the
// JIT lacks between statements). Same wiring as the interruptible stdin / Http
// paths; pulling shared.h keeps proc.h value-neutral (no Value / JitValue).

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
// The POSIX path uses fork/exec/pipe/poll; the Windows path (below the _WIN32
// branches) uses CreateProcess/CreatePipe with per-child reader threads.
#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <algorithm>  // std::min
#include <cctype>     // std::tolower (case-insensitive env-key match)
#include <map>        // live-handle registry keyed by an opaque id
#include <memory>     // unique_ptr — stable buffer address across WinChild moves
#include <mutex>      // guards the live-handle registry across parallel isolates
#include <thread>     // per-child stdout/stderr reader threads
#include <os_compat.h>  // guarded <windows.h> (NOMINMAX + WIN32_LEAN_AND_MEAN)
#endif

#include <shared.h>  // culebra_g_sigint / interrupt_requested / throw_if_interrupted

#if !defined(_WIN32)
extern char** environ;
#endif

namespace culebra::proc {

struct ProcResult {
  int64_t code = 0;     // WEXITSTATUS on normal exit; -1 when killed by signal.
  std::string out;      // full stdout (raw bytes).
  std::string err;      // full stderr (raw bytes).
  bool ok = false;      // exited normally with code 0 (no signal).
  std::string signal;   // signal name ("SIGTERM") if killed; empty otherwise.
  bool timed_out = false;  // true if we killed it for exceeding its timeout.
};

struct RunOutcome {
  bool spawned = false;      // false => failed before/at exec (no result).
  ProcResult result;         // valid only when spawned.
  int err_no = 0;            // errno captured when !spawned.
  std::string err_what;      // failing step ("fork"/"pipe"/"execvp"/"chdir").
};

// A live child (Proc.spawn): the pid plus the out/err fds the caller keeps.
struct SpawnResult {
  bool spawned = false;
  int64_t pid = -1;
  int out_fd = -1, err_fd = -1;
  int err_no = 0;            // errno when !spawned.
  std::string err_what;     // failing step when !spawned.
};

// What environment a child gets. Two independent facts, kept together because
// neither answers the question alone: `vars` is what the caller sets, and
// `inherit` is whether the parent's environment is the base they land on.
// A null EnvSpec* means "inherit the parent unchanged" — the spelling for a
// caller with nothing to say, and the same child environment as {{}, true}.
struct EnvSpec {
  std::vector<std::pair<std::string, std::string>> vars;
  bool inherit = true;

  // Nothing to build when the parent's own block already is the answer.
  bool is_default() const { return inherit && vars.empty(); }
};

#if !defined(_WIN32)
inline std::string signal_name(int sig) {
  switch (sig) {
    case SIGHUP:  return "SIGHUP";
    case SIGINT:  return "SIGINT";
    case SIGQUIT: return "SIGQUIT";
    case SIGILL:  return "SIGILL";
    case SIGTRAP: return "SIGTRAP";
    case SIGABRT: return "SIGABRT";
    case SIGFPE:  return "SIGFPE";
    case SIGKILL: return "SIGKILL";
    case SIGBUS:  return "SIGBUS";
    case SIGSEGV: return "SIGSEGV";
    case SIGSYS:  return "SIGSYS";
    case SIGPIPE: return "SIGPIPE";
    case SIGALRM: return "SIGALRM";
    case SIGTERM: return "SIGTERM";
    default:      return "SIG" + std::to_string(sig);
  }
}
#endif  // !_WIN32

// One-line human reason a (spawned) command counts as a failure, for `check:`
// and `fail_fast:` error messages.
inline std::string failure_detail(const ProcResult& r) {
  if (r.timed_out) return "timed out";
  if (!r.signal.empty()) return "killed by " + r.signal;
  return "exited with code " + std::to_string(r.code);
}

// Same, but for a whole outcome — covers a spawn failure too.
inline std::string outcome_detail(const RunOutcome& oc) {
  if (!oc.spawned)
    return oc.err_what + " failed: " +
           std::system_category().message(oc.err_no);
  return failure_detail(oc.result);
}

namespace _detail {

// run_all's job-scheduling core, shared by the POSIX (`Child`) and Windows
// (`WinChild`) backends: tracks per-command retries and keeps up to `limit`
// children busy, drawing from `retry_queue` first then new work. `ChildT` is
// the platform child handle; `spawn(i)` must launch command `i` (spawn_child
// + any timeout wiring) and return its ChildT — this only sequences *when*,
// not *how*.
template <typename ChildT, typename SpawnFn>
struct RetryScheduler {
  RetryScheduler(size_t n, size_t limit, int64_t retries, bool fail_fast,
                 SpawnFn spawn)
      : limit_(limit), fail_fast_(fail_fast), spawn_(std::move(spawn)),
        results(n), attempts_left_(n, retries < 0 ? 0 : retries) {
    running.reserve(limit);
  }

  std::vector<ChildT> running;
  std::vector<RunOutcome> results;
  size_t finished = 0;
  int64_t failed_index = -1;

  // Record a completed attempt: re-queue it if it failed with retries left,
  // else mark it finished (and arm fail_fast if it's still a failure).
  void handle_outcome(size_t idx, RunOutcome&& oc) {
    bool failed = !oc.spawned || !oc.result.ok;
    results[idx] = std::move(oc);
    if (failed && attempts_left_[idx] > 0) {
      --attempts_left_[idx];
      retry_queue_.push_back(idx);
      return;
    }
    ++finished;
    if (fail_fast_ && failed && failed_index < 0)
      failed_index = static_cast<int64_t>(idx);
  }

  // Keep up to `limit` children busy, drawing from retries first then new work.
  void fill() {
    while (failed_index < 0 && running.size() < limit_ &&
           (!retry_queue_.empty() || next_ < results.size())) {
      if (!retry_queue_.empty()) {
        size_t i = retry_queue_.back();
        retry_queue_.pop_back();
        launch(i);
      } else {
        launch(next_++);
      }
    }
  }

 private:
  void launch(size_t i) {
    ChildT c = spawn_(i);
    if (c.done) {
      handle_outcome(i, std::move(c.outcome));  // spawn failure: instant attempt
    } else {
      running.push_back(std::move(c));
    }
  }

  size_t limit_;
  bool fail_fast_;
  SpawnFn spawn_;
  std::vector<long> attempts_left_;     // remaining retries per command
  std::vector<size_t> retry_queue_;     // indices awaiting a re-run
  size_t next_ = 0;
};

// Deduces SpawnFn from `spawn` so callers only name ChildT explicitly:
// make_retry_scheduler<Child>(n, limit, retries, fail_fast, [&](size_t i){...}).
template <typename ChildT, typename SpawnFn>
inline RetryScheduler<ChildT, SpawnFn> make_retry_scheduler(size_t n,
                                                            size_t limit,
                                                            int64_t retries,
                                                            bool fail_fast,
                                                            SpawnFn spawn) {
  return RetryScheduler<ChildT, SpawnFn>(n, limit, retries, fail_fast,
                                         std::move(spawn));
}

}  // namespace _detail

#if !defined(_WIN32)

// Decode a waitpid() status plus captured output into a ProcResult.
inline ProcResult decode_result(int status, std::string out, std::string err) {
  ProcResult r;
  r.out = std::move(out);
  r.err = std::move(err);
  if (WIFEXITED(status)) {
    r.code = WEXITSTATUS(status);
    r.ok = (r.code == 0);
  } else if (WIFSIGNALED(status)) {
    r.code = -1;
    r.signal = signal_name(WTERMSIG(status));
    r.ok = false;
  }
  return r;
}

namespace _detail {

inline void set_nonblocking(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Monotonic milliseconds, for per-child timeout deadlines.
inline int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Grace period between SIGTERM and SIGKILL when a child overruns its timeout.
inline constexpr long kKillGraceMs = 100;

// Poll loops cap their wait at this many ms so a pending Ctrl+C / isolate cancel
// is honored within ~kInterruptPollMs even when no child fd becomes ready and
// the child does not exit on the signal itself.
inline constexpr int kInterruptPollMs = 100;

// True when a cooperative interrupt (process Ctrl+C or this isolate's cancel) is
// pending. Read-only — the one-shot consume + throw happens via
// throw_if_interrupted() once the blocking loop has killed/reaped its children.
inline bool interrupt_pending() {
  return culebra_g_sigint.load(std::memory_order_relaxed) || interrupt_requested();
}

// Clamp a poll() timeout (ms; -1 == block) so the loop wakes at least every
// kInterruptPollMs to re-check the interrupt flag and re-enforce deadlines.
inline int clamp_interrupt_timeout(int pto) {
  return (pto < 0 || pto > kInterruptPollMs) ? kInterruptPollMs : pto;
}

// Restores the previous SIGPIPE disposition on scope exit. We must ignore
// SIGPIPE while writing a child's stdin: if the child exits early and closes
// its read end, the write would otherwise kill the culebra process. Process-
// global, so this assumes Proc runs on the main thread (true for MVP). One
// guard wraps a whole batch — never nest per child.
struct SigpipeGuard {
  struct sigaction old_sa{};
  bool active = false;
  SigpipeGuard() {
    struct sigaction ign{};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    ign.sa_flags = 0;
    if (sigaction(SIGPIPE, &ign, &old_sa) == 0) active = true;
  }
  ~SigpipeGuard() {
    if (active) sigaction(SIGPIPE, &old_sa, nullptr);
  }
  SigpipeGuard(const SigpipeGuard&) = delete;
  SigpipeGuard& operator=(const SigpipeGuard&) = delete;
};

// One running (or finished) child. Owns its three parent-side fds; move-only
// so fd ownership transfers cleanly (default move would copy fd ints and
// double-close). The destructor only closes fds — reaping is explicit.
struct Child {
  pid_t pid = -1;
  int in_fd = -1, out_fd = -1, err_fd = -1;       // parent ends; -1 == closed.
  bool in_open = false, out_open = false, err_open = false;
  const std::string* stdin_data = nullptr;        // points into caller storage.
  size_t in_off = 0, index = 0;                   // index = position in input.
  std::string out, err;
  RunOutcome outcome;                             // filled on spawn-fail or reap.
  bool done = false;                              // terminal (spawn-fail/reaped).
  long deadline_ms = 0;       // absolute now_ms() deadline; 0 == no timeout.
  long kill_deadline_ms = 0;  // SIGKILL-after-SIGTERM deadline; 0 == not yet sent.
  bool timed_out = false;     // killed for exceeding deadline_ms.

  Child() = default;
  Child(Child&& o) noexcept { *this = std::move(o); }
  Child& operator=(Child&& o) noexcept {
    if (this != &o) {
      pid = o.pid; in_fd = o.in_fd; out_fd = o.out_fd; err_fd = o.err_fd;
      in_open = o.in_open; out_open = o.out_open; err_open = o.err_open;
      stdin_data = o.stdin_data; in_off = o.in_off; index = o.index;
      out = std::move(o.out); err = std::move(o.err);
      outcome = std::move(o.outcome); done = o.done;
      deadline_ms = o.deadline_ms; kill_deadline_ms = o.kill_deadline_ms;
      timed_out = o.timed_out;
      o.pid = -1; o.in_fd = o.out_fd = o.err_fd = -1;
      o.in_open = o.out_open = o.err_open = false;
    }
    return *this;
  }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  ~Child() {
    if (in_fd >= 0) close(in_fd);
    if (out_fd >= 0) close(out_fd);
    if (err_fd >= 0) close(err_fd);
  }
};

// poll() slot indices for one child's open fds within a shared pollfd array.
struct Slots { int out = -1, err = -1, in = -1; };

// Forks+execs one command. On any pre-exec failure returns a Child with
// done=true and outcome.spawned=false (its slot still occupies index). On
// success returns a live Child with out/err_open=true and in_open iff a
// non-empty stdin payload was given.
inline Child spawn_child(
    const std::vector<std::string>& argv,
    const std::string* cwd,
    const EnvSpec* env,
    const std::string* stdin_data,
    size_t index,
    const std::vector<int>* inherit_fds = nullptr) {
  Child c;
  c.index = index;
  c.stdin_data = stdin_data;
  if (argv.empty()) {
    c.outcome.err_no = EINVAL;
    c.outcome.err_what = "argv";
    c.done = true;
    return c;
  }

  // Build argv/envp before fork (no malloc between fork and exec).
  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (const auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
  cargv.push_back(nullptr);

  const bool custom_env = env && !env->is_default();
  std::vector<std::string> env_storage;
  std::vector<char*> cenvp;
  if (custom_env) {
    if (env->inherit) {
      for (char** e = environ; e && *e; ++e) {
        std::string entry(*e);
        auto eq = entry.find('=');
        std::string key = (eq == std::string::npos) ? entry : entry.substr(0, eq);
        bool overridden = false;
        for (const auto& kv : env->vars) {
          if (kv.first == key) { overridden = true; break; }
        }
        if (!overridden) env_storage.push_back(std::move(entry));
      }
    }
    for (const auto& kv : env->vars) {
      env_storage.push_back(kv.first + "=" + kv.second);
    }
    cenvp.reserve(env_storage.size() + 1);
    for (auto& s : env_storage) cenvp.push_back(const_cast<char*>(s.c_str()));
    cenvp.push_back(nullptr);
  }

  int out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
  int in_pipe[2] = {-1, -1}, exec_pipe[2] = {-1, -1};
  auto close_pair = [](int p[2]) {
    if (p[0] >= 0) close(p[0]);
    if (p[1] >= 0) close(p[1]);
  };
  auto fail = [&](const char* what) -> Child {
    c.outcome.err_no = errno;
    c.outcome.err_what = what;
    c.done = true;
    close_pair(out_pipe);
    close_pair(err_pipe);
    close_pair(in_pipe);
    close_pair(exec_pipe);
    return std::move(c);
  };
  if (pipe(out_pipe) != 0) return fail("pipe");
  if (pipe(err_pipe) != 0) return fail("pipe");
  if (pipe(in_pipe) != 0) return fail("pipe");
  if (pipe(exec_pipe) != 0) return fail("pipe");

  // exec_pipe carries the child's errno to the parent on exec/chdir failure;
  // CLOEXEC closes it automatically on a successful exec (parent sees EOF).
  fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);

  pid_t pid = fork();
  if (pid < 0) return fail("fork");

  if (pid == 0) {
    // ---- child: only async-signal-safe calls between fork and exec ----
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(in_pipe[0]);  close(in_pipe[1]);
    close(out_pipe[0]); close(out_pipe[1]);
    close(err_pipe[0]); close(err_pipe[1]);
    close(exec_pipe[0]);
    if (cwd && !cwd->empty()) {
      if (chdir(cwd->c_str()) != 0) {
        int e = errno;
        (void)!write(exec_pipe[1], &e, sizeof(e));
        _exit(127);
      }
    }
    // Opt the shared-memory fds INTO inheritance: they are CLOEXEC in the
    // parent (so they don't leak into unrelated children), so clear it here in
    // the child that the caller chose to share with. fcntl is async-signal-safe.
    if (inherit_fds) {
      for (int sfd : *inherit_fds) {
        if (sfd < 0) continue;
        int fl = fcntl(sfd, F_GETFD, 0);
        if (fl >= 0) fcntl(sfd, F_SETFD, fl & ~FD_CLOEXEC);
      }
    }
    if (custom_env) environ = cenvp.data();
    execvp(cargv[0], cargv.data());
    int e = errno;  // only reached if exec failed
    (void)!write(exec_pipe[1], &e, sizeof(e));
    _exit(127);
  }

  // ---- parent ----
  c.pid = pid;
  close(in_pipe[0]);
  close(out_pipe[1]);
  close(err_pipe[1]);
  close(exec_pipe[1]);

  int child_errno = 0;
  ssize_t er;
  do {
    er = read(exec_pipe[0], &child_errno, sizeof(child_errno));
  } while (er < 0 && errno == EINTR);
  close(exec_pipe[0]);
  if (er == static_cast<ssize_t>(sizeof(child_errno))) {
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(err_pipe[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    c.pid = -1;
    c.outcome.err_no = child_errno;
    c.outcome.err_what = "execvp";
    c.done = true;
    return c;
  }

  // Success: retain the parent ends. CLOEXEC so a later fork (sibling child)
  // does NOT inherit them — otherwise the sibling holds this child's pipe read
  // ends open and this child's EOF never arrives (multi-child deadlock).
  c.out_fd = out_pipe[0];
  c.err_fd = err_pipe[0];
  c.in_fd = in_pipe[1];
  c.out_open = c.err_open = c.in_open = true;
  fcntl(c.out_fd, F_SETFD, FD_CLOEXEC);
  fcntl(c.err_fd, F_SETFD, FD_CLOEXEC);
  fcntl(c.in_fd, F_SETFD, FD_CLOEXEC);
  set_nonblocking(c.out_fd);
  set_nonblocking(c.err_fd);
  set_nonblocking(c.in_fd);
  if (!stdin_data || stdin_data->empty()) {
    close(c.in_fd);
    c.in_fd = -1;
    c.in_open = false;
  }
  return c;
}

// Append a child's open fds to fds[], recording their slot indices.
inline Slots fill_pollfds(Child& c, std::vector<pollfd>& fds) {
  Slots s;
  if (c.out_open) { fds.push_back({c.out_fd, POLLIN, 0});  s.out = (int)fds.size() - 1; }
  if (c.err_open) { fds.push_back({c.err_fd, POLLIN, 0});  s.err = (int)fds.size() - 1; }
  if (c.in_open)  { fds.push_back({c.in_fd, POLLOUT, 0});  s.in = (int)fds.size() - 1; }
  return s;
}

// Consume one child's revents after poll(): drain stdout/stderr, push stdin.
// Closes a fd (and clears its flag) on EOF/error.
inline void apply_revents(Child& c, const Slots& s,
                          const std::vector<pollfd>& fds, char* buf,
                          size_t buflen) {
  auto drain = [&](int idx, int& fd, std::string& dst, bool& open_flag) {
    if (idx < 0) return;
    if (!(fds[idx].revents & (POLLIN | POLLHUP | POLLERR))) return;
    for (;;) {
      ssize_t n = read(fd, buf, buflen);
      if (n > 0) { dst.append(buf, static_cast<size_t>(n)); continue; }
      if (n == 0) { open_flag = false; break; }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      open_flag = false;
      break;
    }
    if (!open_flag) { close(fd); fd = -1; }
  };
  drain(s.out, c.out_fd, c.out, c.out_open);
  drain(s.err, c.err_fd, c.err, c.err_open);

  if (s.in >= 0 && (fds[s.in].revents & (POLLOUT | POLLERR | POLLHUP))) {
    if (fds[s.in].revents & (POLLERR | POLLHUP)) {
      close(c.in_fd);
      c.in_fd = -1;
      c.in_open = false;
    } else {
      const std::string& sd = *c.stdin_data;
      ssize_t n = write(c.in_fd, sd.data() + c.in_off, sd.size() - c.in_off);
      if (n > 0) {
        c.in_off += static_cast<size_t>(n);
        if (c.in_off >= sd.size()) {
          close(c.in_fd);
          c.in_fd = -1;
          c.in_open = false;
        }
      } else if (n < 0 && errno != EINTR && errno != EAGAIN &&
                 errno != EWOULDBLOCK) {
        close(c.in_fd);
        c.in_fd = -1;
        c.in_open = false;
      }
    }
  }
}

// One scheduler step: poll every running child once (up to timeout_ms, -1 =
// block) and drain its revents. Returns false only on a fatal poll() error
// (caller stops); on EINTR or a poll timeout it returns true so the caller can
// recompute deadlines and re-poll. A step with no open fds is a no-op.
inline bool poll_step(std::vector<Child>& running, char* buf, size_t buflen,
                      int timeout_ms) {
  std::vector<pollfd> fds;
  std::vector<Slots> slots(running.size());
  bool any_fd = false;
  for (size_t k = 0; k < running.size(); k++) {
    slots[k] = fill_pollfds(running[k], fds);
    if (running[k].out_open || running[k].err_open || running[k].in_open)
      any_fd = true;
  }
  if (!any_fd) return true;
  int pr = poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
  if (pr < 0) return errno == EINTR;  // EINTR: caller re-polls; else fatal.
  for (size_t k = 0; k < running.size(); k++)
    apply_revents(running[k], slots[k], fds, buf, buflen);  // pr==0 => no-op
  return true;
}

// Enforces per-child timeouts: SIGTERM a child past its deadline, escalate to
// SIGKILL after a grace period. Marks timed_out. Returns the earliest absolute
// wake time across pending deadlines (-1 if none) so the caller can size its
// next poll() wait.
inline int64_t enforce_deadlines(std::vector<Child>& running, int64_t now) {
  int64_t next = -1;
  auto consider = [&](long when) {
    if (when > 0 && (next < 0 || when < next)) next = when;
  };
  for (auto& c : running) {
    if (c.pid <= 0) continue;
    if (c.kill_deadline_ms > 0) {
      if (now >= c.kill_deadline_ms) kill(c.pid, SIGKILL);
      else consider(c.kill_deadline_ms);
    } else if (c.deadline_ms > 0) {
      if (now >= c.deadline_ms) {
        kill(c.pid, SIGTERM);
        c.timed_out = true;
        c.kill_deadline_ms = now + kKillGraceMs;
        consider(c.kill_deadline_ms);
      } else {
        consider(c.deadline_ms);
      }
    }
  }
  return next;
}

// Enforce deadlines now and return the poll() wait (ms) until the next one, or
// -1 if no child has a pending deadline. Wraps the run_command/run_all loops'
// shared now -> enforce -> clamp step.
inline int deadline_poll_timeout(std::vector<Child>& running) {
  int64_t now = now_ms();
  int64_t next = enforce_deadlines(running, now);
  return (next < 0) ? -1 : static_cast<int>(next > now ? next - now : 0);
}

// waitpid the child (EINTR-safe), decode status into outcome, close stray fds.
// Call only once both pipes are EOF. Returns the filled RunOutcome.
inline RunOutcome reap_child(Child& c) {
  if (c.in_fd >= 0)  { close(c.in_fd);  c.in_fd = -1; }
  if (c.out_fd >= 0) { close(c.out_fd); c.out_fd = -1; }
  if (c.err_fd >= 0) { close(c.err_fd); c.err_fd = -1; }
  int status = 0;
  while (waitpid(c.pid, &status, 0) < 0 && errno == EINTR) {}
  c.pid = -1;
  ProcResult r = decode_result(status, std::move(c.out), std::move(c.err));
  r.timed_out = c.timed_out;
  c.outcome.spawned = true;
  c.outcome.result = std::move(r);
  c.done = true;
  return std::move(c.outcome);
}

// SIGKILL every survivor, then reap them all. Two passes so they die in
// parallel rather than serially.
inline void kill_and_reap(std::vector<Child>& cs) {
  for (auto& c : cs) {
    if (c.pid > 0) kill(c.pid, SIGKILL);
    if (c.in_fd >= 0)  { close(c.in_fd);  c.in_fd = -1; }
    if (c.out_fd >= 0) { close(c.out_fd); c.out_fd = -1; }
    if (c.err_fd >= 0) { close(c.err_fd); c.err_fd = -1; }
  }
  for (auto& c : cs) {
    if (c.pid <= 0) continue;
    int st = 0;
    while (waitpid(c.pid, &st, 0) < 0 && errno == EINTR) {}
    c.pid = -1;
  }
}

// Default concurrency = online CPU count, capped so limit*3 fds stay well
// under the typical RLIMIT_NOFILE (256 on macOS).
inline size_t default_limit() {
  int64_t n = sysconf(_SC_NPROCESSORS_ONLN);
  size_t cpu = (n > 0) ? static_cast<size_t>(n) : 4;
  return cpu < 64 ? cpu : 64;
}

// Kills + reaps any still-running children if the scheduler unwinds early
// (exception). The central anti-zombie guarantee; disarm() on clean exit.
struct ScopeKiller {
  std::vector<Child>& running;
  bool armed = true;
  explicit ScopeKiller(std::vector<Child>& r) : running(r) {}
  void disarm() { armed = false; }
  ~ScopeKiller() { if (armed) kill_and_reap(running); }
  ScopeKiller(const ScopeKiller&) = delete;
  ScopeKiller& operator=(const ScopeKiller&) = delete;
};

}  // namespace _detail

// Runs argv[0] (PATH-resolved) synchronously.
//   cwd            : working directory, or nullptr to inherit.
//   env  : the child's environment, or nullptr to inherit the
//                    parent's unchanged. Its vars land on the parent's
//                    environment when EnvSpec::inherit (so PATH survives) and
//                    on an empty one when it does not.
//   stdin_data     : bytes written to the child's stdin, then closed.
//   timeout_ms     : kill (SIGTERM then SIGKILL) the child if it runs longer
//                    than this many ms; 0 == no timeout. A timed-out result has
//                    ok:false and timed_out:true.
inline RunOutcome run_command(
    const std::vector<std::string>& argv,
    const std::string* cwd,
    const EnvSpec* env,
    const std::string& stdin_data,
    int64_t timeout_ms = 0,
    const std::vector<int>* inherit_fds = nullptr) {
  _detail::SigpipeGuard guard;
  const std::string* sp = stdin_data.empty() ? nullptr : &stdin_data;
  _detail::Child c =
      _detail::spawn_child(argv, cwd, env, sp, 0, inherit_fds);
  if (c.done) return std::move(c.outcome);
  if (timeout_ms > 0) c.deadline_ms = _detail::now_ms() + timeout_ms;

  std::vector<_detail::Child> running;
  running.push_back(std::move(c));
  _detail::ScopeKiller killer(running);  // SIGKILL + reap the child if we throw.
  char buf[65536];
  while (running[0].out_open || running[0].err_open || running[0].in_open) {
    if (_detail::interrupt_pending()) throw_if_interrupted();  // killer reaps
    int pto = _detail::clamp_interrupt_timeout(_detail::deadline_poll_timeout(running));
    if (!_detail::poll_step(running, buf, sizeof(buf), pto)) break;
  }
  RunOutcome oc = _detail::reap_child(running[0]);
  killer.disarm();
  throw_if_interrupted();  // a Ctrl+C that landed as the child finished
  return oc;
}

// Runs every command with at most `limit` concurrent children (0 => default).
// allSettled: a failure never aborts the others; a spawn failure is reported
// as RunOutcome.spawned==false at its index. Results are returned in input
// order. cwd/env are shared across the batch; `stdins`, if given, is one
// payload per command. `timeout_ms` (0 == none) applies per command, measured
// from each command's own start.
// `fail_fast`: stop at the first command that fails (spawn failure, non-zero
// exit, signal, or timeout), SIGKILL the rest, and report that command's index
// via `*out_failed`. With fail_fast off (default) it is plain allSettled.
// `retries`: re-run a failed command up to this many extra times; the result is
// the last attempt's. fail_fast only triggers once retries are exhausted.
inline std::vector<RunOutcome> run_all(
    const std::vector<std::vector<std::string>>& commands,
    size_t limit = 0,
    const std::string* cwd = nullptr,
    const EnvSpec* env = nullptr,
    const std::vector<std::string>* stdins = nullptr,
    int64_t timeout_ms = 0,
    bool fail_fast = false,
    size_t* out_failed = nullptr,
    int64_t retries = 0,
    const std::vector<int>* inherit_fds = nullptr) {
  size_t n = commands.size();
  if (n == 0) return {};
  if (limit == 0) limit = _detail::default_limit();
  if (limit > n) limit = n;

  _detail::SigpipeGuard guard;
  auto sched = _detail::make_retry_scheduler<_detail::Child>(
      n, limit, retries, fail_fast, [&](size_t i) {
        const std::string* sp =
            (stdins && !(*stdins)[i].empty()) ? &(*stdins)[i] : nullptr;
        _detail::Child c =
            _detail::spawn_child(commands[i], cwd, env, sp, i, inherit_fds);
        if (!c.done && timeout_ms > 0)
          c.deadline_ms = _detail::now_ms() + timeout_ms;
        return c;
      });
  _detail::ScopeKiller killer(sched.running);
  sched.fill();

  char buf[65536];
  while (sched.finished < n && sched.failed_index < 0) {
    if (_detail::interrupt_pending()) throw_if_interrupted();  // killer reaps survivors
    int pto = _detail::clamp_interrupt_timeout(
        _detail::deadline_poll_timeout(sched.running));
    if (!_detail::poll_step(sched.running, buf, sizeof(buf), pto)) break;
    // Reap finished children (both pipes EOF), then backfill to keep <= limit.
    for (size_t k = 0; k < sched.running.size();) {
      _detail::Child& c = sched.running[k];
      if (!c.out_open && !c.err_open && !c.in_open) {
        size_t idx = c.index;
        RunOutcome oc = _detail::reap_child(c);
        sched.running.erase(sched.running.begin() + k);
        sched.handle_outcome(idx, std::move(oc));
        if (sched.failed_index >= 0) break;
        sched.fill();
      } else {
        ++k;
      }
    }
  }
  // Kills survivors on a fail_fast trigger (or a poll-error break); no-op once
  // everything has been reaped.
  _detail::kill_and_reap(sched.running);
  killer.disarm();
  throw_if_interrupted();  // a Ctrl+C that landed as the batch finished
  if (out_failed && sched.failed_index >= 0)
    *out_failed = static_cast<size_t>(sched.failed_index);
  return std::move(sched.results);
}

// Runs up to `limit` (0 => all) commands and returns the first to finish
// (winner index + its RunOutcome); every other started child is SIGKILLed and
// reaped before returning. A command that fails to spawn finishes instantly
// and can win. Empty input => {SIZE_MAX, RunOutcome{}}.
inline std::pair<size_t, RunOutcome> run_race(
    const std::vector<std::vector<std::string>>& commands,
    size_t limit = 0,
    const std::string* cwd = nullptr,
    const EnvSpec* env = nullptr,
    const std::vector<std::string>* stdins = nullptr,
    const std::vector<int>* inherit_fds = nullptr) {
  size_t n = commands.size();
  if (n == 0) return {SIZE_MAX, RunOutcome{}};
  if (limit == 0 || limit > n) limit = n;

  _detail::SigpipeGuard guard;
  std::vector<_detail::Child> running;
  running.reserve(limit);
  _detail::ScopeKiller killer(running);

  size_t winner = SIZE_MAX;
  RunOutcome win_oc;
  for (size_t i = 0; i < limit && winner == SIZE_MAX; i++) {
    const std::string* sp =
        (stdins && !(*stdins)[i].empty()) ? &(*stdins)[i] : nullptr;
    _detail::Child c =
        _detail::spawn_child(commands[i], cwd, env, sp, i, inherit_fds);
    if (c.done) {
      winner = i;
      win_oc = std::move(c.outcome);
    } else {
      running.push_back(std::move(c));
    }
  }

  char buf[65536];
  while (winner == SIZE_MAX && !running.empty()) {
    if (_detail::interrupt_pending()) throw_if_interrupted();  // killer reaps survivors
    if (!_detail::poll_step(running, buf, sizeof(buf),
                            _detail::clamp_interrupt_timeout(-1)))
      break;
    for (size_t k = 0; k < running.size(); k++) {
      _detail::Child& c = running[k];
      if (!c.out_open && !c.err_open && !c.in_open) {
        winner = c.index;
        win_oc = _detail::reap_child(c);
        running.erase(running.begin() + k);
        break;
      }
    }
  }

  _detail::kill_and_reap(running);
  killer.disarm();
  throw_if_interrupted();  // a Ctrl+C that landed as the winner finished
  return {winner, std::move(win_oc)};
}

#else  // _WIN32 — CreateProcess/CreatePipe port. Each child gets a reader thread
       // per output pipe so a large stdout and a stdin feed cannot deadlock (the
       // POSIX path multiplexes the three fds with poll() on one thread instead).
       // inherit_fds (cross-process SharedBuffer) is Phase 3, so it is ignored.

namespace _detail {

// long long, not long: on LLP64 Windows `long` is 32-bit and would truncate the
// 64-bit millisecond count (wrapping ~every 24.8 days of uptime).
inline long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
inline constexpr int kInterruptPollMs = 100;

inline bool interrupt_pending() {
  return culebra_g_sigint.load(std::memory_order_relaxed) || interrupt_requested();
}
inline int clamp_interrupt_timeout(long long pto) {
  return (pto < 0 || pto > kInterruptPollMs) ? kInterruptPollMs
                                             : static_cast<int>(pto);
}

// Close-and-null and join-the-two-readers are the invariants WinChild and
// WinLive both compose, so they live as free helpers here.
inline void close_handle(HANDLE& h) {
  if (h) { CloseHandle(h); h = nullptr; }
}
inline void join_readers(std::thread& a, std::thread& b) {
  if (a.joinable()) a.join();
  if (b.joinable()) b.join();
}

// argv -> one command line, quoted per the CommandLineToArgvW rules the child's
// CRT startup uses to split it back into argv. (Backslashes only matter before a
// quote or at the string's end, hence the run counting.)
inline std::string quote_arg(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
  std::string out = "\"";
  size_t bs = 0;
  for (char c : a) {
    if (c == '\\') { bs++; continue; }
    if (c == '"') { out.append(bs * 2 + 1, '\\'); out.push_back('"'); bs = 0; continue; }
    out.append(bs, '\\'); bs = 0; out.push_back(c);
  }
  out.append(bs * 2, '\\');
  out.push_back('"');
  return out;
}
inline std::string build_command_line(const std::vector<std::string>& argv) {
  std::string cl;
  for (size_t i = 0; i < argv.size(); i++) {
    if (i) cl.push_back(' ');
    cl += quote_arg(argv[i]);
  }
  return cl;
}

// Environment block ("K=V\0K=V\0\0") for `spec`: its vars, laid on the parent's
// block when `spec->inherit` and on nothing when it does not (Windows env keys
// are case-insensitive). Empty return => caller passes NULL so the child
// inherits the parent environment unchanged.
inline std::string build_env_block(const EnvSpec* spec) {
  if (!spec || spec->is_default()) return {};
  auto ieq = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
      if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
        return false;
    return true;
  };
  std::string block;
  if (spec->inherit) {
    char* env = GetEnvironmentStringsA();
    for (char* p = env; env && *p;) {
      std::string entry(p);
      p += entry.size() + 1;
      // "=X:=..." drive-letter cwd vars start with '=' — keep them verbatim.
      if (!entry.empty() && entry[0] != '=') {
        auto eq = entry.find('=');
        std::string key = eq == std::string::npos ? entry : entry.substr(0, eq);
        bool overridden = false;
        for (auto& kv : spec->vars)
          if (ieq(kv.first, key)) { overridden = true; break; }
        if (overridden) continue;
      }
      block += entry;
      block.push_back('\0');
    }
    if (env) FreeEnvironmentStringsA(env);
  }
  for (auto& kv : spec->vars) {
    block += kv.first; block.push_back('='); block += kv.second;
    block.push_back('\0');
  }
  // Block terminator: each entry already ends in a NUL, and the block ends in
  // one more. An environment with no entries at all has no first NUL to borrow,
  // so it needs both written here to come out as CreateProcess's "\0\0".
  if (block.empty()) block.push_back('\0');
  block.push_back('\0');
  return block;
}

// Low-level spawn: create the three redirected pipes, CreateProcess, close the
// child's ends in the parent, and hand back the process handle + parent-side
// read ends + stdin write end. Does NOT start reader threads or feed stdin — the
// caller does that against a stable buffer address (blocking child vs live
// registry entry). ok=false with err_no/err_what on any failure.
struct SpawnHandles {
  HANDLE hProcess = nullptr, out_rd = nullptr, err_rd = nullptr, in_wr = nullptr;
  bool ok = false;
  int err_no = 0;
  const char* err_what = "";
};
inline SpawnHandles spawn_process(
    const std::vector<std::string>& argv, const std::string* cwd,
    const EnvSpec* env) {
  SpawnHandles h;
  if (argv.empty()) { h.err_no = ERROR_INVALID_PARAMETER; h.err_what = "argv"; return h; }

  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out_rd = nullptr, out_wr = nullptr, err_rd = nullptr, err_wr = nullptr,
         in_rd = nullptr, in_wr = nullptr;
  auto fail = [&](const char* what) -> SpawnHandles {
    h.err_no = (int)GetLastError(); h.err_what = what;
    close_handle(out_rd); close_handle(out_wr); close_handle(err_rd);
    close_handle(err_wr); close_handle(in_rd); close_handle(in_wr);
    return h;
  };
  if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) return fail("pipe");
  if (!CreatePipe(&err_rd, &err_wr, &sa, 0)) return fail("pipe");
  if (!CreatePipe(&in_rd, &in_wr, &sa, 0)) return fail("pipe");
  // The parent ends must not be inherited by the child — else the child keeps a
  // copy open and the pipe never reports EOF to the reader.
  SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);

  std::string cmdline_s = build_command_line(argv);
  std::vector<char> cmdline(cmdline_s.begin(), cmdline_s.end());
  cmdline.push_back('\0');  // CreateProcessA may write into this buffer.
  std::string envblock = build_env_block(env);
  std::string cwds = cwd ? *cwd : std::string();

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_rd; si.hStdOutput = out_wr; si.hStdError = err_wr;
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW,
                           envblock.empty() ? nullptr : envblock.data(),
                           cwds.empty() ? nullptr : cwds.c_str(), &si, &pi);
  if (!ok) return fail("CreateProcess");
  CloseHandle(pi.hThread);
  close_handle(out_wr); close_handle(err_wr);
  close_handle(in_rd);  // child's ends, in the parent.

  h.hProcess = pi.hProcess; h.out_rd = out_rd; h.err_rd = err_rd; h.in_wr = in_wr;
  h.ok = true;
  return h;
}

// Drain a pipe to EOF into `dst`. Its own thread, so stdout, stderr and the
// parent's stdin write all make progress — the reader-thread analogue of the
// POSIX poll() multiplexing. `dst` must outlive the thread (heap-stable).
inline void drain_pipe(HANDLE h, std::string* dst) {
  char buf[65536];
  DWORD n = 0;
  while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) dst->append(buf, n);
}

// Feed the whole stdin payload then close so the child sees EOF (MVP one-shot
// feed, matching the POSIX path). Safe once the reader threads are running.
inline void feed_and_close_stdin(HANDLE in_wr, const std::string* sd) {
  if (sd && !sd->empty()) {
    size_t off = 0;
    while (off < sd->size()) {
      DWORD w = 0;
      DWORD chunk = (DWORD)std::min<size_t>(sd->size() - off, (size_t)1 << 20);
      if (!WriteFile(in_wr, sd->data() + off, chunk, &w, nullptr) || w == 0)
        break;  // child closed its stdin early.
      off += w;
    }
  }
  CloseHandle(in_wr);
}

// One running (or spawn-failed) child. out/err live on the heap so the reader
// threads keep a valid pointer across WinChild moves (vector growth, erase).
// Move-only; the dtor joins the readers and closes the handles.
struct WinChild {
  HANDLE hProcess = nullptr, out_rd = nullptr, err_rd = nullptr;
  std::unique_ptr<std::string> out = std::make_unique<std::string>();
  std::unique_ptr<std::string> err = std::make_unique<std::string>();
  std::thread out_th, err_th;
  size_t index = 0;
  long long deadline_ms = 0;  // absolute now_ms() deadline; 0 == none.
  bool timed_out = false;
  bool done = false;  // spawn-failed: outcome holds the reason, no process.
  RunOutcome outcome;

  WinChild() = default;
  WinChild(WinChild&& o) noexcept { *this = std::move(o); }
  // A defaulted move would COPY the raw HANDLEs and leave them set in the
  // source, so the moved-from object's destructor closes handles the moved-to
  // object still owns — the exact bug that made a moved WinChild's hProcess
  // invalid (WaitForMultipleObjects then spins on a dead handle). Transfer the
  // handles and null the source, as the POSIX Child does with its fds.
  WinChild& operator=(WinChild&& o) noexcept {
    if (this != &o) {
      join_readers();   // our threads must be non-joinable before overwriting
      close_handles();  // and our handles released before taking o's
      hProcess = o.hProcess; out_rd = o.out_rd; err_rd = o.err_rd;
      out = std::move(o.out); err = std::move(o.err);
      out_th = std::move(o.out_th); err_th = std::move(o.err_th);
      index = o.index; deadline_ms = o.deadline_ms;
      timed_out = o.timed_out; done = o.done; outcome = std::move(o.outcome);
      o.hProcess = o.out_rd = o.err_rd = nullptr;
    }
    return *this;
  }
  WinChild(const WinChild&) = delete;
  WinChild& operator=(const WinChild&) = delete;
  ~WinChild() { join_readers(); close_handles(); }

  void join_readers() { _detail::join_readers(out_th, err_th); }
  void close_handles() {
    close_handle(out_rd); close_handle(err_rd); close_handle(hProcess);
  }
};

inline WinChild spawn_child(
    const std::vector<std::string>& argv, const std::string* cwd,
    const EnvSpec* env,
    const std::string* stdin_data, size_t index) {
  WinChild c;
  c.index = index;
  SpawnHandles h = spawn_process(argv, cwd, env);
  if (!h.ok) {
    c.outcome.err_no = h.err_no; c.outcome.err_what = h.err_what; c.done = true;
    return c;
  }
  c.hProcess = h.hProcess; c.out_rd = h.out_rd; c.err_rd = h.err_rd;
  c.out_th = std::thread(drain_pipe, h.out_rd, c.out.get());
  c.err_th = std::thread(drain_pipe, h.err_rd, c.err.get());
  feed_and_close_stdin(h.in_wr, stdin_data);
  return c;
}

inline ProcResult decode_child(WinChild& c) {
  ProcResult r;
  r.out = std::move(*c.out);
  r.err = std::move(*c.err);
  // TerminateProcess is the only kill Windows has: immediate and uncatchable,
  // no cooperative phase — the POSIX side's own escalation (SIGTERM, then
  // SIGKILL on a grace-period timeout) reports whichever signal actually
  // ended the child, and the closer analogue here is the one the target
  // cannot ignore.
  if (c.timed_out) {
    r.code = -1; r.ok = false; r.timed_out = true; r.signal = "SIGKILL";
    return r;
  }
  DWORD code = 0;
  GetExitCodeProcess(c.hProcess, &code);
  r.code = (long)code;
  r.ok = (code == 0);
  return r;
}

// Join readers + decode the exit code into a RunOutcome. Consumes the child.
inline RunOutcome reap_child(WinChild& c) {
  c.join_readers();
  RunOutcome oc;
  oc.spawned = true;
  oc.result = decode_child(c);
  return oc;
}

inline void kill_and_reap(std::vector<WinChild>& cs) {
  for (auto& c : cs)
    if (c.hProcess) TerminateProcess(c.hProcess, 1);
  for (auto& c : cs) c.join_readers();  // dtor closes handles.
}

// TerminateProcess any child past its deadline (flagging it timed_out); return
// the ms until the nearest remaining deadline, or -1 when none is pending.
inline long long enforce_deadlines(std::vector<WinChild>& running, long long now) {
  long long nearest = -1;
  for (auto& c : running) {
    if (c.deadline_ms == 0 || c.timed_out) continue;
    if (now >= c.deadline_ms) {
      if (c.hProcess) TerminateProcess(c.hProcess, 1);
      c.timed_out = true;
    } else {
      long long rem = c.deadline_ms - now;
      if (nearest < 0 || rem < nearest) nearest = rem;
    }
  }
  return nearest;
}

// Block up to timeout_ms until any running child exits. Caps the wait set at
// MAXIMUM_WAIT_OBJECTS; the caller rescans all children for exit afterwards, so
// a child beyond the cap is still reaped on a later pass.
inline void wait_any(std::vector<WinChild>& running, DWORD timeout_ms) {
  std::vector<HANDLE> handles;
  for (auto& c : running) {
    if (c.hProcess) handles.push_back(c.hProcess);
    if (handles.size() >= MAXIMUM_WAIT_OBJECTS) break;
  }
  if (handles.empty()) { Sleep(timeout_ms); return; }
  WaitForMultipleObjects((DWORD)handles.size(), handles.data(), FALSE, timeout_ms);
}

inline bool child_exited(WinChild& c) {
  return c.timed_out ||
         (c.hProcess && WaitForSingleObject(c.hProcess, 0) == WAIT_OBJECT_0);
}

inline size_t default_limit() {
  unsigned hc = std::thread::hardware_concurrency();
  return hc ? hc : 4;
}

// SIGKILL + reap survivors if the scope is left by exception.
struct ScopeKiller {
  std::vector<WinChild>& running;
  bool armed = true;
  explicit ScopeKiller(std::vector<WinChild>& r) : running(r) {}
  void disarm() { armed = false; }
  ~ScopeKiller() { if (armed) kill_and_reap(running); }
  ScopeKiller(const ScopeKiller&) = delete;
  ScopeKiller& operator=(const ScopeKiller&) = delete;
};

}  // namespace _detail

inline RunOutcome run_command(
    const std::vector<std::string>& argv, const std::string* cwd,
    const EnvSpec* env,
    const std::string& stdin_data, int64_t timeout_ms = 0,
    const std::vector<int>* = nullptr) {
  const std::string* sp = stdin_data.empty() ? nullptr : &stdin_data;
  _detail::WinChild c = _detail::spawn_child(argv, cwd, env, sp, 0);
  if (c.done) return std::move(c.outcome);
  if (timeout_ms > 0) c.deadline_ms = _detail::now_ms() + timeout_ms;

  std::vector<_detail::WinChild> running;
  running.push_back(std::move(c));
  _detail::ScopeKiller killer(running);
  while (!_detail::child_exited(running[0])) {
    if (_detail::interrupt_pending()) throw_if_interrupted();  // killer reaps
    long long nearest = _detail::enforce_deadlines(running, _detail::now_ms());
    if (running[0].timed_out) break;
    _detail::wait_any(running, (DWORD)_detail::clamp_interrupt_timeout(nearest));
  }
  RunOutcome oc = _detail::reap_child(running[0]);
  killer.disarm();
  throw_if_interrupted();
  return oc;
}

inline std::vector<RunOutcome> run_all(
    const std::vector<std::vector<std::string>>& commands, size_t limit = 0,
    const std::string* cwd = nullptr,
    const EnvSpec* env = nullptr,
    const std::vector<std::string>* stdins = nullptr, int64_t timeout_ms = 0,
    bool fail_fast = false, size_t* out_failed = nullptr, int64_t retries = 0,
    const std::vector<int>* = nullptr) {
  size_t n = commands.size();
  if (n == 0) return {};
  if (limit == 0) limit = _detail::default_limit();
  if (limit > n) limit = n;

  auto sched = _detail::make_retry_scheduler<_detail::WinChild>(
      n, limit, retries, fail_fast, [&](size_t i) {
        const std::string* sp =
            (stdins && !(*stdins)[i].empty()) ? &(*stdins)[i] : nullptr;
        _detail::WinChild c = _detail::spawn_child(commands[i], cwd, env, sp, i);
        if (!c.done && timeout_ms > 0)
          c.deadline_ms = _detail::now_ms() + timeout_ms;
        return c;
      });
  _detail::ScopeKiller killer(sched.running);
  sched.fill();

  while (sched.finished < n && sched.failed_index < 0) {
    if (_detail::interrupt_pending()) throw_if_interrupted();  // killer reaps
    long long nearest =
        _detail::enforce_deadlines(sched.running, _detail::now_ms());
    _detail::wait_any(sched.running,
                      (DWORD)_detail::clamp_interrupt_timeout(nearest));
    for (size_t k = 0; k < sched.running.size();) {
      if (_detail::child_exited(sched.running[k])) {
        size_t idx = sched.running[k].index;
        RunOutcome oc = _detail::reap_child(sched.running[k]);
        sched.running.erase(sched.running.begin() + k);
        sched.handle_outcome(idx, std::move(oc));
        if (sched.failed_index >= 0) break;
        sched.fill();
      } else {
        ++k;
      }
    }
  }
  _detail::kill_and_reap(sched.running);
  killer.disarm();
  throw_if_interrupted();
  if (out_failed && sched.failed_index >= 0)
    *out_failed = (size_t)sched.failed_index;
  return std::move(sched.results);
}

inline std::pair<size_t, RunOutcome> run_race(
    const std::vector<std::vector<std::string>>& commands, size_t limit = 0,
    const std::string* cwd = nullptr,
    const EnvSpec* env = nullptr,
    const std::vector<std::string>* stdins = nullptr,
    const std::vector<int>* = nullptr) {
  size_t n = commands.size();
  if (n == 0) return {SIZE_MAX, RunOutcome{}};
  if (limit == 0 || limit > n) limit = n;

  std::vector<_detail::WinChild> running;
  running.reserve(limit);
  _detail::ScopeKiller killer(running);
  size_t winner = SIZE_MAX;
  RunOutcome win_oc;
  for (size_t i = 0; i < limit && winner == SIZE_MAX; i++) {
    const std::string* sp =
        (stdins && !(*stdins)[i].empty()) ? &(*stdins)[i] : nullptr;
    _detail::WinChild c = _detail::spawn_child(commands[i], cwd, env, sp, i);
    if (c.done) {
      winner = i;
      win_oc = std::move(c.outcome);
    } else {
      running.push_back(std::move(c));
    }
  }
  while (winner == SIZE_MAX && !running.empty()) {
    if (_detail::interrupt_pending()) throw_if_interrupted();  // killer reaps
    _detail::wait_any(running, (DWORD)_detail::kInterruptPollMs);
    for (size_t k = 0; k < running.size(); k++) {
      if (_detail::child_exited(running[k])) {
        winner = running[k].index;
        win_oc = _detail::reap_child(running[k]);
        running.erase(running.begin() + k);
        break;
      }
    }
  }
  _detail::kill_and_reap(running);
  killer.disarm();
  throw_if_interrupted();
  return {winner, std::move(win_oc)};
}

#endif  // !_WIN32

// --- Live handle primitives (Proc.spawn). The child outlives the call; the
// caller (a culebra handle object) keeps pid + out/err fds and later drains
// + reaps them via wait_handle / poll_handle / kill_pid. ---

#if !defined(_WIN32)

// Spawns a child without waiting: writes stdin_data then closes the child's
// stdin, and returns the live pid + out/err fds (parent keeps them open). On a
// spawn failure returns spawned=false with errno/step.
inline SpawnResult spawn_detached(
    const std::vector<std::string>& argv, const std::string* cwd,
    const EnvSpec* env,
    const std::string& stdin_data,
    const std::vector<int>* inherit_fds = nullptr) {
  _detail::SigpipeGuard guard;
  const std::string* sp = stdin_data.empty() ? nullptr : &stdin_data;
  _detail::Child c = _detail::spawn_child(argv, cwd, env, sp, 0,
                                          inherit_fds);
  SpawnResult sr;
  if (c.done) {
    sr.err_no = c.outcome.err_no;
    sr.err_what = c.outcome.err_what;
    return sr;
  }
  // Write the whole stdin payload, then close (MVP: a one-shot feed at spawn).
  if (c.in_open && c.stdin_data) {
    const std::string& sd = *c.stdin_data;
    size_t off = 0;
    while (off < sd.size()) {
      ssize_t w = write(c.in_fd, sd.data() + off, sd.size() - off);
      if (w > 0) { off += static_cast<size_t>(w); continue; }
      if (w < 0 && errno == EINTR) continue;
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        struct pollfd pf{c.in_fd, POLLOUT, 0};
        poll(&pf, 1, -1);
        continue;
      }
      break;  // EPIPE etc. — child closed its stdin early.
    }
  }
  if (c.in_fd >= 0) { close(c.in_fd); c.in_fd = -1; c.in_open = false; }

  sr.spawned = true;
  sr.pid = c.pid;
  sr.out_fd = c.out_fd;
  sr.err_fd = c.err_fd;
  // Detach fds + pid from the Child so its destructor leaves them to the handle.
  c.out_fd = c.err_fd = -1;
  c.out_open = c.err_open = false;
  c.pid = -1;
  return sr;
}

inline void kill_pid(int64_t pid, int sig) {
  if (pid > 0) kill(static_cast<pid_t>(pid), sig);
}

// Non-blocking: returns true and fills `status` if the child has exited (and
// reaps it); false if it is still running.
inline bool try_reap(int64_t pid, int& status) {
  pid_t r;
  do {
    r = waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
  } while (r < 0 && errno == EINTR);
  return r == static_cast<pid_t>(pid);
}

// Adopts pid + out/err fds into a Child to reuse the drain+reap machinery.
inline _detail::Child _adopt(int64_t pid, int& out_fd, int& err_fd) {
  _detail::Child c;
  c.pid = static_cast<pid_t>(pid);
  c.out_fd = out_fd;
  c.err_fd = err_fd;
  c.out_open = out_fd >= 0;
  c.err_open = err_fd >= 0;
  out_fd = err_fd = -1;  // ownership moves into the Child.
  return c;
}

// Blocking: drains out/err to EOF and waitpid()s -> full ProcResult. Consumes
// the fds (sets them to -1).
inline ProcResult wait_handle(int64_t pid, int& out_fd, int& err_fd) {
  std::vector<_detail::Child> running;
  running.push_back(_adopt(pid, out_fd, err_fd));
  char buf[65536];
  while (running[0].out_open || running[0].err_open) {
    if (!_detail::poll_step(running, buf, sizeof(buf), -1)) break;
  }
  return std::move(_detail::reap_child(running[0]).result);
}

// After try_reap() has reaped the child: drain remaining buffered out/err and
// decode `status` into a ProcResult. Consumes the fds.
inline ProcResult drain_reaped(int status, int& out_fd, int& err_fd) {
  std::vector<_detail::Child> running;
  running.push_back(_adopt(0, out_fd, err_fd));  // pid 0: no waitpid here.
  char buf[65536];
  while (running[0].out_open || running[0].err_open) {
    if (!_detail::poll_step(running, buf, sizeof(buf), -1)) break;
  }
  return decode_result(status, std::move(running[0].out),
                       std::move(running[0].err));
}

#else  // _WIN32 — live-handle (Proc.spawn) primitives. The child outlives the
       // call; its reader threads keep draining into a registry entry that the
       // interpreter reattaches to via an opaque id (stored as _pid and _out).

namespace _detail {
// A spawned child living past the call. Its buffers are stable (std::map is
// node-allocated), so the reader threads keep a valid pointer to them.
struct WinLive {
  HANDLE hProcess = nullptr, out_rd = nullptr, err_rd = nullptr;
  std::string out, err;
  std::thread out_th, err_th;
};
inline std::map<long, WinLive>& live_registry() {
  static std::map<long, WinLive> m;
  return m;
}
inline int64_t& live_next_id() {
  static int64_t id = 1;
  return id;
}
// Serialises the id counter + map structure across parallel isolates (two
// Isolate/Parallel threads may each Proc.spawn concurrently). Held only for the
// counter/find/insert/erase, never across a blocking wait or a reader join — the
// node the map hands back is address-stable, so the value can be touched outside
// the lock. The POSIX path needs none of this: its pid/fds are stateless OS
// resources carried in the handle object.
inline std::mutex& live_mutex() {
  static std::mutex m;
  return m;
}
inline void live_join_close(WinLive& lv) {
  join_readers(lv.out_th, lv.err_th);
  close_handle(lv.out_rd); close_handle(lv.err_rd); close_handle(lv.hProcess);
}
}  // namespace _detail

// Spawn without waiting: feed+close stdin, keep the process + reader threads
// alive in the registry, and return an opaque id. It rides in pid, and out_fd
// mirrors it so drain_reaped (which is handed only the fds) finds the entry too.
inline SpawnResult spawn_detached(
    const std::vector<std::string>& argv, const std::string* cwd,
    const EnvSpec* env,
    const std::string& stdin_data, const std::vector<int>* = nullptr) {
  SpawnResult sr;
  _detail::SpawnHandles h = _detail::spawn_process(argv, cwd, env);
  if (!h.ok) { sr.err_no = h.err_no; sr.err_what = h.err_what; return sr; }
  int64_t id;
  _detail::WinLive* lv;
  {
    std::lock_guard<std::mutex> g(_detail::live_mutex());
    id = _detail::live_next_id()++;
    lv = &_detail::live_registry()[id];  // address-stable map node
  }
  lv->hProcess = h.hProcess; lv->out_rd = h.out_rd; lv->err_rd = h.err_rd;
  lv->out_th = std::thread(_detail::drain_pipe, h.out_rd, &lv->out);
  lv->err_th = std::thread(_detail::drain_pipe, h.err_rd, &lv->err);
  const std::string* sp = stdin_data.empty() ? nullptr : &stdin_data;
  _detail::feed_and_close_stdin(h.in_wr, sp);
  sr.spawned = true;
  sr.pid = id;
  sr.out_fd = (int)id;  // drain_reaped keys off the fd; err_fd is unused.
  sr.err_fd = -1;
  return sr;
}

inline void kill_pid(int64_t id, int /*sig*/) {
  std::lock_guard<std::mutex> g(_detail::live_mutex());
  auto& reg = _detail::live_registry();
  auto it = reg.find(id);
  if (it != reg.end() && it->second.hProcess)
    TerminateProcess(it->second.hProcess, 1);
}

// Non-blocking: true (and fills `status` with the exit code) once the child has
// exited; false while it runs. Leaves the entry in place — drain_reaped drops it.
inline bool try_reap(int64_t id, int& status) {
  std::lock_guard<std::mutex> g(_detail::live_mutex());
  auto& reg = _detail::live_registry();
  auto it = reg.find(id);
  if (it == reg.end() || !it->second.hProcess) { status = 0; return true; }
  if (WaitForSingleObject(it->second.hProcess, 0) != WAIT_OBJECT_0) return false;
  DWORD code = 0;
  GetExitCodeProcess(it->second.hProcess, &code);
  status = (int)code;
  return true;
}

// Blocking: wait for exit, drain out/err to EOF, decode, and drop the entry.
// `id` arrives as pid. The lock is dropped across the INFINITE wait so other
// handle ops make progress; the map node stays valid (single-owner id).
inline ProcResult wait_handle(int64_t id, int&, int&) {
  ProcResult r;
  _detail::WinLive* lv = nullptr;
  {
    std::lock_guard<std::mutex> g(_detail::live_mutex());
    auto& reg = _detail::live_registry();
    auto it = reg.find(id);
    if (it == reg.end()) return r;
    lv = &it->second;
  }
  DWORD code = 0;
  if (lv->hProcess) {
    WaitForSingleObject(lv->hProcess, INFINITE);
    GetExitCodeProcess(lv->hProcess, &code);
  }
  _detail::live_join_close(*lv);
  r.out = std::move(lv->out);
  r.err = std::move(lv->err);
  r.code = (long)code;
  r.ok = (code == 0);
  {
    std::lock_guard<std::mutex> g(_detail::live_mutex());
    _detail::live_registry().erase(id);
  }
  return r;
}

// After try_reap() reported exit: drain the remaining buffered out/err and
// decode `status` into a ProcResult. `id` arrives as out_fd.
inline ProcResult drain_reaped(int status, int& out_fd, int&) {
  ProcResult r;
  _detail::WinLive* lv = nullptr;
  {
    std::lock_guard<std::mutex> g(_detail::live_mutex());
    auto& reg = _detail::live_registry();
    auto it = reg.find((long)out_fd);
    if (it != reg.end()) lv = &it->second;
  }
  if (lv) {
    _detail::live_join_close(*lv);
    r.out = std::move(lv->out);
    r.err = std::move(lv->err);
    std::lock_guard<std::mutex> g(_detail::live_mutex());
    _detail::live_registry().erase((long)out_fd);
  }
  r.code = status;
  r.ok = (status == 0);
  return r;
}

#endif  // !_WIN32

}  // namespace culebra::proc
