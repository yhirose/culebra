#pragma once

// Type-neutral POSIX process execution core for Proc.run.
//
// No dependency on culebra Value / JitValue / GC: both the interp and JIT
// backends call run_command() and adapt RunOutcome into their own object
// representation. Keeping this header value-neutral lets stdlib_interp.h and
// stdlib_jit.h include it without pulling each other in.
//
// run_command() runs one external command synchronously (blocking), captures
// stdout/stderr without deadlocking (poll-multiplexed drain), optionally feeds
// stdin, and reports signal death distinctly from a normal exit code.

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/wait.h>
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

namespace _detail {

inline void set_nonblocking(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Restores the previous SIGPIPE disposition on scope exit. We must ignore
// SIGPIPE while writing the child's stdin: if the child exits early and closes
// its read end, the write would otherwise kill the culebra process. Process-
// global, so this assumes Proc.run runs on the main thread (true for MVP).
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

}  // namespace _detail

// Runs argv[0] (PATH-resolved) synchronously.
//   cwd            : working directory, or nullptr to inherit.
//   env_overrides  : key/value pairs merged onto the parent environment
//                    (parent kept so PATH survives), or nullptr to inherit.
//   stdin_data     : bytes written to the child's stdin, then closed.
// timeout_ms is reserved for a future kwarg and currently ignored.
inline RunOutcome run_command(
    const std::vector<std::string>& argv,
    const std::string* cwd,
    const std::vector<std::pair<std::string, std::string>>* env_overrides,
    const std::string& stdin_data,
    long /*timeout_ms*/ = 0) {
  RunOutcome oc;
  if (argv.empty()) {
    oc.err_no = EINVAL;
    oc.err_what = "argv";
    return oc;
  }

  _detail::SigpipeGuard sigpipe_guard;

  // Build the argv pointer array before fork (no malloc between fork/exec).
  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (const auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
  cargv.push_back(nullptr);

  // Build a merged envp before fork when overrides are present. Start from the
  // parent environ (so PATH etc. survive — execvp needs PATH), drop any keys
  // the caller overrides, then append the overrides.
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
  auto fail_pipe = [&](void) {
    oc.err_no = errno;
    oc.err_what = "pipe";
    close_pair(out_pipe);
    close_pair(err_pipe);
    close_pair(in_pipe);
    close_pair(exec_pipe);
    return oc;
  };
  if (pipe(out_pipe) != 0) return fail_pipe();
  if (pipe(err_pipe) != 0) return fail_pipe();
  if (pipe(in_pipe) != 0) return fail_pipe();
  if (pipe(exec_pipe) != 0) return fail_pipe();

  // The exec_pipe carries the child's errno to the parent if exec/chdir fails;
  // CLOEXEC closes it automatically on a successful exec (parent sees EOF).
  fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);

  pid_t pid = fork();
  if (pid < 0) {
    oc.err_no = errno;
    oc.err_what = "fork";
    close_pair(out_pipe);
    close_pair(err_pipe);
    close_pair(in_pipe);
    close_pair(exec_pipe);
    return oc;
  }

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
  close(in_pipe[0]);  in_pipe[0] = -1;
  close(out_pipe[1]); out_pipe[1] = -1;
  close(err_pipe[1]); err_pipe[1] = -1;
  close(exec_pipe[1]); exec_pipe[1] = -1;

  // exec/chdir failure: a readable exec_pipe means the child wrote its errno.
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
    oc.err_no = child_errno;
    oc.err_what = "execvp";
    return oc;
  }

  // Drain stdout/stderr and feed stdin via a single poll loop so neither side
  // can deadlock on a full pipe buffer.
  _detail::set_nonblocking(out_pipe[0]);
  _detail::set_nonblocking(err_pipe[0]);
  _detail::set_nonblocking(in_pipe[1]);

  std::string out, err;
  size_t in_off = 0;
  bool out_open = true, err_open = true, in_open = true;
  if (stdin_data.empty()) {
    close(in_pipe[1]);
    in_pipe[1] = -1;
    in_open = false;
  }

  char buf[65536];
  while (out_open || err_open || in_open) {
    struct pollfd fds[3];
    int idx_out = -1, idx_err = -1, idx_in = -1, nfds = 0;
    if (out_open) { fds[nfds] = {out_pipe[0], POLLIN, 0};  idx_out = nfds++; }
    if (err_open) { fds[nfds] = {err_pipe[0], POLLIN, 0};  idx_err = nfds++; }
    if (in_open)  { fds[nfds] = {in_pipe[1], POLLOUT, 0};  idx_in = nfds++; }

    int pr = poll(fds, nfds, -1);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }

    auto drain = [&](int idx, int fd, std::string& dst, bool& open_flag) {
      if (idx < 0) return;
      if (!(fds[idx].revents & (POLLIN | POLLHUP | POLLERR))) return;
      for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) { dst.append(buf, static_cast<size_t>(n)); continue; }
        if (n == 0) { open_flag = false; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        open_flag = false;
        break;
      }
    };
    drain(idx_out, out_pipe[0], out, out_open);
    drain(idx_err, err_pipe[0], err, err_open);

    if (idx_in >= 0 && (fds[idx_in].revents & (POLLOUT | POLLERR | POLLHUP))) {
      if (fds[idx_in].revents & (POLLERR | POLLHUP)) {
        close(in_pipe[1]);
        in_pipe[1] = -1;
        in_open = false;
      } else {
        ssize_t n = write(in_pipe[1], stdin_data.data() + in_off,
                          stdin_data.size() - in_off);
        if (n > 0) {
          in_off += static_cast<size_t>(n);
          if (in_off >= stdin_data.size()) {
            close(in_pipe[1]);
            in_pipe[1] = -1;
            in_open = false;
          }
        } else if (n < 0 && errno != EINTR && errno != EAGAIN &&
                   errno != EWOULDBLOCK) {
          close(in_pipe[1]);
          in_pipe[1] = -1;
          in_open = false;
        }
      }
    }
  }

  if (in_pipe[1] >= 0) close(in_pipe[1]);
  close(out_pipe[0]);
  close(err_pipe[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

  ProcResult r;
  r.out = std::move(out);
  r.err = std::move(err);
  if (WIFEXITED(status)) {
    r.code = WEXITSTATUS(status);
    r.signal.clear();
    r.ok = (r.code == 0);
  } else if (WIFSIGNALED(status)) {
    r.code = -1;
    r.signal = signal_name(WTERMSIG(status));
    r.ok = false;
  }
  oc.spawned = true;
  oc.result = std::move(r);
  return oc;
}

}  // namespace culebra::proc
