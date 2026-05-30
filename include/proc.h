#pragma once

// Type-neutral POSIX process execution core for the Proc namespace.
//
// No dependency on culebra Value / JitValue / GC: both the interp and JIT
// backends call these and adapt RunOutcome into their own object
// representation. Keeping this header value-neutral lets stdlib_interp.h and
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

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace culebra::proc {

struct ProcResult {
  long code = 0;        // WEXITSTATUS on normal exit; -1 when killed by signal.
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

inline void set_nonblocking(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Monotonic milliseconds, for per-child timeout deadlines.
inline long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Grace period between SIGTERM and SIGKILL when a child overruns its timeout.
inline constexpr long kKillGraceMs = 100;

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
    const std::vector<std::pair<std::string, std::string>>* env_overrides,
    const std::string* stdin_data,
    size_t index) {
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

  const bool custom_env = (env_overrides != nullptr);
  std::vector<std::string> env_storage;
  std::vector<char*> cenvp;
  if (custom_env) {
    for (char** e = environ; e && *e; ++e) {
      std::string entry(*e);
      auto eq = entry.find('=');
      std::string key = (eq == std::string::npos) ? entry : entry.substr(0, eq);
      bool overridden = false;
      for (const auto& kv : *env_overrides) {
        if (kv.first == key) { overridden = true; break; }
      }
      if (!overridden) env_storage.push_back(std::move(entry));
    }
    for (const auto& kv : *env_overrides) {
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
inline long enforce_deadlines(std::vector<Child>& running, long now) {
  long next = -1;
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
  long now = now_ms();
  long next = enforce_deadlines(running, now);
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
  ProcResult r;
  r.out = std::move(c.out);
  r.err = std::move(c.err);
  if (WIFEXITED(status)) {
    r.code = WEXITSTATUS(status);
    r.ok = (r.code == 0);
  } else if (WIFSIGNALED(status)) {
    r.code = -1;
    r.signal = signal_name(WTERMSIG(status));
    r.ok = false;
  }
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
  long n = sysconf(_SC_NPROCESSORS_ONLN);
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
//   env_overrides  : key/value pairs merged onto the parent environment
//                    (parent kept so PATH survives), or nullptr to inherit.
//   stdin_data     : bytes written to the child's stdin, then closed.
//   timeout_ms     : kill (SIGTERM then SIGKILL) the child if it runs longer
//                    than this many ms; 0 == no timeout. A timed-out result has
//                    ok:false and timed_out:true.
inline RunOutcome run_command(
    const std::vector<std::string>& argv,
    const std::string* cwd,
    const std::vector<std::pair<std::string, std::string>>* env_overrides,
    const std::string& stdin_data,
    long timeout_ms = 0) {
  _detail::SigpipeGuard guard;
  const std::string* sp = stdin_data.empty() ? nullptr : &stdin_data;
  _detail::Child c =
      _detail::spawn_child(argv, cwd, env_overrides, sp, 0);
  if (c.done) return std::move(c.outcome);
  if (timeout_ms > 0) c.deadline_ms = _detail::now_ms() + timeout_ms;

  std::vector<_detail::Child> running;
  running.push_back(std::move(c));
  char buf[65536];
  while (running[0].out_open || running[0].err_open || running[0].in_open) {
    int pto = _detail::deadline_poll_timeout(running);
    if (!_detail::poll_step(running, buf, sizeof(buf), pto)) break;
  }
  return _detail::reap_child(running[0]);
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
inline std::vector<RunOutcome> run_all(
    const std::vector<std::vector<std::string>>& commands,
    size_t limit = 0,
    const std::string* cwd = nullptr,
    const std::vector<std::pair<std::string, std::string>>* env = nullptr,
    const std::vector<std::string>* stdins = nullptr,
    long timeout_ms = 0,
    bool fail_fast = false,
    size_t* out_failed = nullptr) {
  size_t n = commands.size();
  std::vector<RunOutcome> results(n);
  if (n == 0) return results;
  if (limit == 0) limit = _detail::default_limit();
  if (limit > n) limit = n;

  _detail::SigpipeGuard guard;
  std::vector<_detail::Child> running;
  running.reserve(limit);
  _detail::ScopeKiller killer(running);
  size_t next = 0, finished = 0;
  long failed_index = -1;
  auto is_failure = [](const RunOutcome& oc) {
    return !oc.spawned || !oc.result.ok;
  };

  auto launch = [&](size_t i) {
    const std::string* sp =
        (stdins && !(*stdins)[i].empty()) ? &(*stdins)[i] : nullptr;
    _detail::Child c = _detail::spawn_child(commands[i], cwd, env, sp, i);
    if (c.done) {
      results[i] = std::move(c.outcome);
      ++finished;
      if (fail_fast && failed_index < 0 && is_failure(results[i]))
        failed_index = static_cast<long>(i);
    } else {
      if (timeout_ms > 0) c.deadline_ms = _detail::now_ms() + timeout_ms;
      running.push_back(std::move(c));
    }
  };
  while (next < limit) launch(next++);

  char buf[65536];
  while (finished < n && failed_index < 0) {
    int pto = _detail::deadline_poll_timeout(running);
    if (!_detail::poll_step(running, buf, sizeof(buf), pto)) break;
    // Reap finished children (both pipes EOF), then backfill to keep <= limit.
    for (size_t k = 0; k < running.size();) {
      _detail::Child& c = running[k];
      if (!c.out_open && !c.err_open && !c.in_open) {
        size_t idx = c.index;
        results[idx] = _detail::reap_child(c);
        ++finished;
        running.erase(running.begin() + k);
        if (fail_fast && failed_index < 0 && is_failure(results[idx])) {
          failed_index = static_cast<long>(idx);
          break;
        }
        if (failed_index < 0 && next < n) launch(next++);
      } else {
        ++k;
      }
    }
  }
  // Kills survivors on a fail_fast trigger (or a poll-error break); no-op once
  // everything has been reaped.
  _detail::kill_and_reap(running);
  killer.disarm();
  if (out_failed && failed_index >= 0) *out_failed = static_cast<size_t>(failed_index);
  return results;
}

// Runs up to `limit` (0 => all) commands and returns the first to finish
// (winner index + its RunOutcome); every other started child is SIGKILLed and
// reaped before returning. A command that fails to spawn finishes instantly
// and can win. Empty input => {SIZE_MAX, RunOutcome{}}.
inline std::pair<size_t, RunOutcome> run_race(
    const std::vector<std::vector<std::string>>& commands,
    size_t limit = 0,
    const std::string* cwd = nullptr,
    const std::vector<std::pair<std::string, std::string>>* env = nullptr,
    const std::vector<std::string>* stdins = nullptr) {
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
    _detail::Child c = _detail::spawn_child(commands[i], cwd, env, sp, i);
    if (c.done) {
      winner = i;
      win_oc = std::move(c.outcome);
    } else {
      running.push_back(std::move(c));
    }
  }

  char buf[65536];
  while (winner == SIZE_MAX && !running.empty()) {
    if (!_detail::poll_step(running, buf, sizeof(buf), -1)) break;
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
  return {winner, std::move(win_oc)};
}

}  // namespace culebra::proc
