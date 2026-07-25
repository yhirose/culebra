#pragma once

// Type-neutral TCP/UDP socket core for the Net namespace.
//
// No dependency on culebra Value / JitValue / GC: the interp and JIT backends
// both drive these int64 handle ids and adapt the results into their own object
// representation (mirrors proc.h / http.h / sqlite.h). Keeping the header
// value-neutral lets stdlib_interp.h and stdlib_jit.h include it without
// pulling each other in.
//
//   connect / listen / accept            — TCP, blocking with an optional
//                                          per-handle timeout.
//   read / read_line / read_exact /      — stream I/O over a per-socket read
//   write_all / shutdown_write             buffer that lives HERE, not in the
//                                          backends, so the three backends
//                                          cannot drift on framing behaviour.
//   udp_open / udp_send_to /             — connectionless datagrams.
//   udp_recv_from
//   resolve                              — hostname -> addresses (getaddrinfo).
//
// Every blocking wait goes through wait_ready(), which polls in
// kInterruptPollMs slices and calls throw_if_interrupted() — so one Ctrl+C
// stops a blocked accept/read symmetrically across interp/JIT/AOT rather than
// relying on a post-call safepoint (which the JIT lacks between statements).
// Same wiring as proc.h and the interruptible Http paths; pulling shared.h
// keeps this header value-neutral (no Value / JitValue).
//
// Sockets are non-blocking underneath and every operation is `wait_ready()
// then retry`: a poll/select readiness hint is advisory (it can be spurious),
// so a blocking recv() behind it could still stall past its timeout.
//
// Handles are int64 ids into a thread_local table, never a raw fd: a forged id
// is bounds-checked and degrades to a graceful error (same soundness posture as
// sqlite.h and File's fd table). Thread_local is correct because a Net handle
// is __nonsendable__ — it never crosses an isolate, i.e. never crosses a
// thread.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
// winsock2 must precede <windows.h> (os_compat.h) or the ancient winsock.h
// declarations leak in and collide.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>  // std::once_flag for the one-time WSAStartup

#include <os_compat.h>  // guarded <windows.h> (NOMINMAX + WIN32_LEAN_AND_MEAN)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cerrno>

#include <shared.h>  // throw_if_interrupted / culebra_g_sigint (Ctrl+C wiring)

namespace culebra::net {

// Outcome of any blocking operation. The backends map these to culebra
// semantics: Eof to "" / nil, Timeout and Error to NetError.
enum class IoStatus { Ok, Eof, Timeout, Error };

// What a handle id refers to. Checked on every lookup so a Listener id passed
// to a stream op is a graceful error, not a type-confused syscall.
enum class Kind : int { Tcp, Listener, Udp };

#if defined(_WIN32)
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

// Blocking waits wake at least this often to honor a pending Ctrl+C / isolate
// cancel even when the socket never becomes ready (matches proc.h).
inline constexpr long kInterruptPollMs = 100;

// One recv() worth of bytes appended to a socket's read buffer.
inline constexpr size_t kReadChunk = 64 * 1024;

// Default listen backlog. Larger than SOMAXCONN on some platforms is clamped by
// the kernel anyway.
inline constexpr int kDefaultBacklog = 128;

// ---- Platform shims --------------------------------------------------------

namespace detail {

// Networking is unavailable in the Emscripten (Playground) build — the browser
// has no raw sockets. Every entry point reports that up front instead of
// hanging or half-working on emulated calls.
inline bool unavailable(std::string* err) {
#if defined(__EMSCRIPTEN__)
  if (err) *err = "networking is not available in this build";
  return true;
#else
  (void)err;
  return false;
#endif
}

inline int last_error() {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

inline std::string error_string(int code) {
  return std::system_category().message(code);
}

inline bool is_eintr(int e) {
#if defined(_WIN32)
  return e == WSAEINTR;
#else
  return e == EINTR;
#endif
}

inline bool is_wouldblock(int e) {
#if defined(_WIN32)
  return e == WSAEWOULDBLOCK;
#else
  return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

// A non-blocking connect() that has not completed yet.
inline bool is_in_progress(int e) {
#if defined(_WIN32)
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
  return e == EINPROGRESS;
#endif
}

// Winsock needs one process-wide startup before any socket call; every entry
// point that creates a socket or resolves a name calls this first.
inline void platform_init() {
#if defined(_WIN32)
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

inline void close_fd(socket_t fd) {
#if defined(_WIN32)
  ::closesocket(fd);
#else
  ::close(fd);
#endif
}

inline void set_nonblocking(socket_t fd) {
#if defined(_WIN32)
  u_long on = 1;
  ::ioctlsocket(fd, FIONBIO, &on);
#else
  int fl = ::fcntl(fd, F_GETFL, 0);
  if (fl != -1) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

// Keep a dead peer from killing the process with SIGPIPE. BSD/macOS carry the
// per-socket option; Linux has none and uses MSG_NOSIGNAL on each send instead
// (kSendFlags below).
inline void suppress_sigpipe(socket_t fd) {
#if defined(SO_NOSIGPIPE)
  int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&on),
               sizeof(on));
#else
  (void)fd;
#endif
}

inline constexpr int kSendFlags =
#if defined(MSG_NOSIGNAL)
    MSG_NOSIGNAL;
#else
    0;
#endif

// Wait for readiness on one socket. Windows uses select() rather than WSAPoll()
// because WSAPoll does not report a failed non-blocking connect (a long-standing
// winsock defect); select's exceptfds does. Only ever one descriptor, so
// FD_SETSIZE is not a concern.
inline int poll_one(socket_t fd, bool want_write, int timeout_ms) {
#if defined(_WIN32)
  fd_set ready, except;
  FD_ZERO(&ready);
  FD_SET(fd, &ready);
  FD_ZERO(&except);
  FD_SET(fd, &except);
  timeval tv{static_cast<long>(timeout_ms / 1000),
             static_cast<long>((timeout_ms % 1000) * 1000)};
  return ::select(0, want_write ? nullptr : &ready, want_write ? &ready : nullptr,
                  &except, &tv);
#else
  pollfd p{fd, static_cast<short>(want_write ? POLLOUT : POLLIN), 0};
  return ::poll(&p, 1, timeout_ms);
#endif
}

inline long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Absolute deadline for a timeout in ms; <= 0 means wait forever.
inline long deadline_from(long timeout_ms) {
  return timeout_ms > 0 ? now_ms() + timeout_ms : -1;
}

// Block until `fd` is ready (or the deadline passes), yielding to a pending
// Ctrl+C / isolate cancel every kInterruptPollMs. `deadline` is an absolute
// now_ms() value; negative waits forever.
inline IoStatus wait_ready(socket_t fd, bool want_write, long deadline,
                           std::string* err) {
  for (;;) {
    throw_if_interrupted();  // cooperative Interrupted, before another sleep
    long slice = kInterruptPollMs;
    if (deadline >= 0) {
      long left = deadline - now_ms();
      if (left <= 0) return IoStatus::Timeout;
      if (left < slice) slice = left;
    }
    int rc = poll_one(fd, want_write, static_cast<int>(slice));
    if (rc > 0) return IoStatus::Ok;
    if (rc == 0) continue;  // slice expired: re-check interrupt and deadline
    int e = last_error();
    if (is_eintr(e)) continue;
    if (err) *err = error_string(e);
    return IoStatus::Error;
  }
}

// Owns a raw socket until it is either interned into the handle table or the
// scope exits — including via the cooperative Interrupted thrown out of
// wait_ready(), which is exactly the path a hand-rolled close() would miss.
struct FdGuard {
  socket_t fd;
  explicit FdGuard(socket_t f) : fd(f) {}
  ~FdGuard() {
    if (fd != kInvalidSocket) close_fd(fd);
  }
  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;
  socket_t release() {
    socket_t f = fd;
    fd = kInvalidSocket;
    return f;
  }
};

struct AddrInfoGuard {
  addrinfo* p = nullptr;
  AddrInfoGuard() = default;
  ~AddrInfoGuard() {
    if (p) ::freeaddrinfo(p);
  }
  AddrInfoGuard(const AddrInfoGuard&) = delete;
  AddrInfoGuard& operator=(const AddrInfoGuard&) = delete;
};

inline std::string gai_message(int code) {
#if defined(_WIN32)
  return error_string(code);  // getaddrinfo returns a winsock error code
#else
  return ::gai_strerror(code);
#endif
}

// Numeric presentation form of a resolved address ("93.184.216.34" / "::1").
inline bool addr_to_host_port(const sockaddr* sa, socklen_t len,
                              std::string& host, int& port) {
  char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
  int rc = ::getnameinfo(sa, len, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf),
                         NI_NUMERICHOST | NI_NUMERICSERV);
  if (rc != 0) return false;
  host = hbuf;
  port = std::atoi(sbuf);
  return true;
}

// ---- Handle table ----------------------------------------------------------

struct Sock {
  Kind kind = Kind::Tcp;
  socket_t fd = kInvalidSocket;
  long timeout_ms = 0;   // 0 = wait forever (still interruptible)
  std::string rbuf;      // bytes read from the wire, not yet handed to script
  size_t rpos = 0;       // consumed prefix of rbuf
  bool eof = false;      // peer closed its write end and rbuf is drained
};

// unique_ptr so a live Sock& is not invalidated by a concurrent intern() that
// grows the table.
inline thread_local std::vector<std::unique_ptr<Sock>> g_socks;
inline thread_local std::vector<int64_t> g_free;

inline int64_t intern(std::unique_ptr<Sock> s) {
  if (!g_free.empty()) {
    int64_t id = g_free.back();
    g_free.pop_back();
    g_socks[static_cast<size_t>(id)] = std::move(s);
    return id;
  }
  g_socks.push_back(std::move(s));
  return static_cast<int64_t>(g_socks.size()) - 1;
}

inline Sock* find(int64_t id) {
  if (id < 0 || id >= static_cast<int64_t>(g_socks.size())) return nullptr;
  return g_socks[static_cast<size_t>(id)].get();
}

// Look up a live handle of the expected kind. A closed / forged / wrong-kind id
// is a graceful error, never a dereference.
inline Sock* get(int64_t id, Kind want, std::string* err) {
  Sock* s = find(id);
  if (!s) {
    if (err) *err = "operation on a closed socket";
    return nullptr;
  }
  if (s->kind != want) {
    if (err) *err = "wrong socket kind for this operation";
    return nullptr;
  }
  return s;
}

inline size_t avail(const Sock& s) { return s.rbuf.size() - s.rpos; }

// Drop the consumed prefix so a long-lived socket's buffer doesn't grow without
// bound.
inline void compact(Sock& s) {
  if (s.rpos == 0) return;
  s.rbuf.erase(0, s.rpos);
  s.rpos = 0;
}

// One recv() into the socket's buffer, waiting up to its timeout. Ok means
// bytes were appended.
inline IoStatus fill(Sock& s, std::string* err) {
  if (s.eof) return IoStatus::Eof;
  long deadline = deadline_from(s.timeout_ms);
  char tmp[kReadChunk];
  for (;;) {
    IoStatus st = wait_ready(s.fd, false, deadline, err);
    if (st != IoStatus::Ok) return st;
#if defined(_WIN32)
    int n = ::recv(s.fd, tmp, static_cast<int>(sizeof(tmp)), 0);
#else
    ssize_t n = ::recv(s.fd, tmp, sizeof(tmp), 0);
#endif
    if (n > 0) {
      compact(s);
      s.rbuf.append(tmp, static_cast<size_t>(n));
      return IoStatus::Ok;
    }
    if (n == 0) {
      s.eof = true;
      return IoStatus::Eof;
    }
    int e = last_error();
    if (is_eintr(e) || is_wouldblock(e)) continue;  // spurious readiness
    if (err) *err = error_string(e);
    return IoStatus::Error;
  }
}

}  // namespace detail

// ---- Lifecycle -------------------------------------------------------------

// Close a handle of any kind and free its slot. Idempotent; a stale or forged
// id is ignored (mirrors File's close).
inline void close_handle(int64_t id) {
  detail::Sock* s = detail::find(id);
  if (!s) return;
  if (s->fd != kInvalidSocket) detail::close_fd(s->fd);
  detail::g_socks[static_cast<size_t>(id)].reset();
  detail::g_free.push_back(id);
}

inline bool is_open(int64_t id) { return detail::find(id) != nullptr; }

// ---- Options / addresses ---------------------------------------------------

// Set the per-handle blocking timeout in ms; 0 waits forever. Applies to
// connect-time (passed separately), read/write, accept and recv_from.
inline bool set_timeout(int64_t id, long ms, std::string* err) {
  detail::Sock* s = detail::find(id);
  if (!s) {
    if (err) *err = "operation on a closed socket";
    return false;
  }
  s->timeout_ms = ms > 0 ? ms : 0;
  return true;
}

inline long get_timeout(int64_t id) {
  detail::Sock* s = detail::find(id);
  return s ? s->timeout_ms : 0;
}

inline bool set_nodelay(int64_t id, bool on, std::string* err) {
  detail::Sock* s = detail::get(id, Kind::Tcp, err);
  if (!s) return false;
  int v = on ? 1 : 0;
  if (::setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&v), sizeof(v)) != 0) {
    if (err) *err = detail::error_string(detail::last_error());
    return false;
  }
  return true;
}

inline bool set_broadcast(int64_t id, bool on, std::string* err) {
  detail::Sock* s = detail::get(id, Kind::Udp, err);
  if (!s) return false;
  int v = on ? 1 : 0;
  if (::setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&v), sizeof(v)) != 0) {
    if (err) *err = detail::error_string(detail::last_error());
    return false;
  }
  return true;
}

// The address this handle is bound to. For a listener bound to port 0 this is
// how the script learns the ephemeral port it actually got.
inline bool local_addr(int64_t id, std::string& host, int& port,
                       std::string* err) {
  detail::Sock* s = detail::find(id);
  if (!s) {
    if (err) *err = "operation on a closed socket";
    return false;
  }
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (::getsockname(s->fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0 ||
      !detail::addr_to_host_port(reinterpret_cast<sockaddr*>(&ss), len, host,
                                 port)) {
    if (err) *err = detail::error_string(detail::last_error());
    return false;
  }
  return true;
}

inline bool peer_addr(int64_t id, std::string& host, int& port,
                      std::string* err) {
  detail::Sock* s = detail::get(id, Kind::Tcp, err);
  if (!s) return false;
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (::getpeername(s->fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0 ||
      !detail::addr_to_host_port(reinterpret_cast<sockaddr*>(&ss), len, host,
                                 port)) {
    if (err) *err = detail::error_string(detail::last_error());
    return false;
  }
  return true;
}

// ---- DNS -------------------------------------------------------------------

// Resolve `host` to its numeric addresses, in the order the resolver returned
// them, deduplicated. False with *err on a lookup failure.
inline bool resolve(const std::string& host, std::vector<std::string>& out,
                    std::string* err) {
  out.clear();
  if (detail::unavailable(err)) return false;
  detail::platform_init();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;  // one entry per address, not per protocol
  detail::AddrInfoGuard res;
  int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res.p);
  if (rc != 0) {
    if (err) *err = detail::gai_message(rc);
    return false;
  }
  for (addrinfo* ai = res.p; ai; ai = ai->ai_next) {
    std::string h;
    int p = 0;
    if (!detail::addr_to_host_port(ai->ai_addr,
                                   static_cast<socklen_t>(ai->ai_addrlen), h, p))
      continue;
    if (std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
  }
  if (out.empty()) {
    if (err) *err = "no addresses found";
    return false;
  }
  return true;
}

// ---- TCP client ------------------------------------------------------------

// Connect to host:port, trying each resolved address in turn. `timeout_ms`
// bounds each attempt (0 = wait forever) and becomes the handle's I/O timeout.
// Returns a Tcp handle id, or -1 with *err set.
inline int64_t connect(const std::string& host, int port, long timeout_ms,
                       std::string* err) {
  if (detail::unavailable(err)) return -1;
  detail::platform_init();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  detail::AddrInfoGuard res;
  std::string service = std::to_string(port);
  int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res.p);
  if (rc != 0) {
    if (err) *err = detail::gai_message(rc);
    return -1;
  }
  std::string last = "connect failed";
  for (addrinfo* ai = res.p; ai; ai = ai->ai_next) {
    detail::FdGuard fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (fd.fd == kInvalidSocket) {
      last = detail::error_string(detail::last_error());
      continue;
    }
    detail::suppress_sigpipe(fd.fd);
    detail::set_nonblocking(fd.fd);
    if (::connect(fd.fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) !=
        0) {
      int e = detail::last_error();
      if (!detail::is_in_progress(e)) {
        last = detail::error_string(e);
        continue;
      }
      IoStatus st = detail::wait_ready(fd.fd, /*want_write=*/true,
                                       detail::deadline_from(timeout_ms), &last);
      if (st == IoStatus::Timeout) {
        last = "connection timed out";
        continue;
      }
      if (st != IoStatus::Ok) continue;
      int so_err = 0;
      socklen_t len = sizeof(so_err);
      ::getsockopt(fd.fd, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&so_err), &len);
      if (so_err != 0) {
        last = detail::error_string(so_err);
        continue;
      }
    }
    auto s = std::make_unique<detail::Sock>();
    s->kind = Kind::Tcp;
    s->fd = fd.release();
    s->timeout_ms = timeout_ms > 0 ? timeout_ms : 0;
    return detail::intern(std::move(s));
  }
  if (err) *err = last;
  return -1;
}

// ---- TCP server ------------------------------------------------------------

// Bind and listen on host:port. `port` 0 asks the OS for an ephemeral port
// (read it back with local_addr). Returns a Listener handle id, or -1 with
// *err set.
inline int64_t listen(const std::string& host, int port, int backlog,
                      std::string* err) {
  if (detail::unavailable(err)) return -1;
  detail::platform_init();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  detail::AddrInfoGuard res;
  std::string service = std::to_string(port);
  const char* node = host.empty() ? nullptr : host.c_str();
  int rc = ::getaddrinfo(node, service.c_str(), &hints, &res.p);
  if (rc != 0) {
    if (err) *err = detail::gai_message(rc);
    return -1;
  }
  std::string last = "bind failed";
  for (addrinfo* ai = res.p; ai; ai = ai->ai_next) {
    detail::FdGuard fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (fd.fd == kInvalidSocket) {
      last = detail::error_string(detail::last_error());
      continue;
    }
    // Rebind a port still in TIME_WAIT from a previous run rather than failing.
    int on = 1;
    ::setsockopt(fd.fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&on), sizeof(on));
    if (::bind(fd.fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) != 0) {
      last = detail::error_string(detail::last_error());
      continue;
    }
    if (::listen(fd.fd, backlog > 0 ? backlog : kDefaultBacklog) != 0) {
      last = detail::error_string(detail::last_error());
      continue;
    }
    detail::set_nonblocking(fd.fd);
    auto s = std::make_unique<detail::Sock>();
    s->kind = Kind::Listener;
    s->fd = fd.release();
    return detail::intern(std::move(s));
  }
  if (err) *err = last;
  return -1;
}

// Accept one connection, waiting up to the listener's timeout. On Ok, *out_id
// is a fresh Tcp handle that inherits the listener's timeout.
inline IoStatus accept(int64_t lid, int64_t* out_id, std::string* err) {
  detail::Sock* l = detail::get(lid, Kind::Listener, err);
  if (!l) return IoStatus::Error;
  long deadline = detail::deadline_from(l->timeout_ms);
  for (;;) {
    IoStatus st = detail::wait_ready(l->fd, false, deadline, err);
    if (st != IoStatus::Ok) return st;
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    detail::FdGuard fd(
        ::accept(l->fd, reinterpret_cast<sockaddr*>(&ss), &len));
    if (fd.fd == kInvalidSocket) {
      int e = detail::last_error();
      if (detail::is_eintr(e) || detail::is_wouldblock(e)) continue;
      if (err) *err = detail::error_string(e);
      return IoStatus::Error;
    }
    detail::suppress_sigpipe(fd.fd);
    detail::set_nonblocking(fd.fd);
    auto s = std::make_unique<detail::Sock>();
    s->kind = Kind::Tcp;
    s->timeout_ms = l->timeout_ms;
    s->fd = fd.release();
    *out_id = detail::intern(std::move(s));
    return IoStatus::Ok;
  }
}

// ---- Stream I/O ------------------------------------------------------------

// Up to `n` bytes. Ok with a short read is normal (whatever has arrived); Eof
// with `out` empty means the peer closed.
inline IoStatus read(int64_t id, size_t n, std::string& out, std::string* err) {
  out.clear();
  detail::Sock* s = detail::get(id, Kind::Tcp, err);
  if (!s) return IoStatus::Error;
  if (detail::avail(*s) == 0) {
    IoStatus st = detail::fill(*s, err);
    if (st != IoStatus::Ok) return st;
  }
  size_t take = std::min(n, detail::avail(*s));
  out.assign(s->rbuf, s->rpos, take);
  s->rpos += take;
  return IoStatus::Ok;
}

// Exactly `n` bytes. Eof means the peer closed early — `out` then holds the
// partial tail so the caller can report how much it got.
inline IoStatus read_exact(int64_t id, size_t n, std::string& out,
                           std::string* err) {
  out.clear();
  detail::Sock* s = detail::get(id, Kind::Tcp, err);
  if (!s) return IoStatus::Error;
  while (detail::avail(*s) < n) {
    IoStatus st = detail::fill(*s, err);
    if (st == IoStatus::Eof) {
      out.assign(s->rbuf, s->rpos, detail::avail(*s));
      s->rpos = s->rbuf.size();
      return IoStatus::Eof;
    }
    if (st != IoStatus::Ok) return st;
  }
  out.assign(s->rbuf, s->rpos, n);
  s->rpos += n;
  return IoStatus::Ok;
}

// One line, terminator stripped. Unlike File.lines (which also splits on a lone
// \r), a socket line ends at \n only: a bare \r would otherwise force a
// one-byte lookahead that can block mid-stream when a CRLF arrives split across
// two packets. A trailing \r is removed, so CRLF protocols read cleanly.
// Eof means the stream ended with nothing buffered; a final unterminated line
// is returned as Ok.
inline IoStatus read_line(int64_t id, std::string& out, std::string* err) {
  out.clear();
  detail::Sock* s = detail::get(id, Kind::Tcp, err);
  if (!s) return IoStatus::Error;
  for (;;) {
    size_t nl = s->rbuf.find('\n', s->rpos);
    if (nl != std::string::npos) {
      size_t end = nl;
      if (end > s->rpos && s->rbuf[end - 1] == '\r') end--;
      out.assign(s->rbuf, s->rpos, end - s->rpos);
      s->rpos = nl + 1;
      return IoStatus::Ok;
    }
    IoStatus st = detail::fill(*s, err);
    if (st == IoStatus::Eof) {
      if (detail::avail(*s) == 0) return IoStatus::Eof;
      size_t end = s->rbuf.size();
      if (end > s->rpos && s->rbuf[end - 1] == '\r') end--;
      out.assign(s->rbuf, s->rpos, end - s->rpos);
      s->rpos = s->rbuf.size();
      return IoStatus::Ok;
    }
    if (st != IoStatus::Ok) return st;
  }
}

// Write every byte, waiting up to the socket's timeout for each chunk.
inline IoStatus write_all(int64_t id, const char* data, size_t n,
                          std::string* err) {
  detail::Sock* s = detail::get(id, Kind::Tcp, err);
  if (!s) return IoStatus::Error;
  long deadline = detail::deadline_from(s->timeout_ms);
  size_t sent = 0;
  while (sent < n) {
    IoStatus st = detail::wait_ready(s->fd, /*want_write=*/true, deadline, err);
    if (st != IoStatus::Ok) return st;
#if defined(_WIN32)
    int w = ::send(s->fd, data + sent, static_cast<int>(n - sent),
                   detail::kSendFlags);
#else
    ssize_t w = ::send(s->fd, data + sent, n - sent, detail::kSendFlags);
#endif
    if (w > 0) {
      sent += static_cast<size_t>(w);
      continue;
    }
    int e = detail::last_error();
    if (detail::is_eintr(e) || detail::is_wouldblock(e)) continue;
    if (err) *err = detail::error_string(e);
    return IoStatus::Error;
  }
  return IoStatus::Ok;
}

// Half-close: signal EOF to the peer while still reading its reply (the
// request/response idiom of line protocols and HTTP/1.0).
inline bool shutdown_write(int64_t id, std::string* err) {
  detail::Sock* s = detail::get(id, Kind::Tcp, err);
  if (!s) return false;
#if defined(_WIN32)
  int how = SD_SEND;
#else
  int how = SHUT_WR;
#endif
  if (::shutdown(s->fd, how) != 0) {
    if (err) *err = detail::error_string(detail::last_error());
    return false;
  }
  return true;
}

// ---- UDP -------------------------------------------------------------------

// Open a datagram socket bound to host:port (port 0 = ephemeral, host empty =
// all interfaces). Returns a Udp handle id, or -1 with *err set.
inline int64_t udp_open(const std::string& host, int port, std::string* err) {
  if (detail::unavailable(err)) return -1;
  detail::platform_init();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;
  detail::AddrInfoGuard res;
  std::string service = std::to_string(port);
  const char* node = host.empty() ? nullptr : host.c_str();
  int rc = ::getaddrinfo(node, service.c_str(), &hints, &res.p);
  if (rc != 0) {
    if (err) *err = detail::gai_message(rc);
    return -1;
  }
  std::string last = "bind failed";
  for (addrinfo* ai = res.p; ai; ai = ai->ai_next) {
    detail::FdGuard fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (fd.fd == kInvalidSocket) {
      last = detail::error_string(detail::last_error());
      continue;
    }
    int on = 1;
    ::setsockopt(fd.fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&on), sizeof(on));
    if (::bind(fd.fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) != 0) {
      last = detail::error_string(detail::last_error());
      continue;
    }
    detail::suppress_sigpipe(fd.fd);
    detail::set_nonblocking(fd.fd);
    auto s = std::make_unique<detail::Sock>();
    s->kind = Kind::Udp;
    s->fd = fd.release();
    return detail::intern(std::move(s));
  }
  if (err) *err = last;
  return -1;
}

// Send one datagram to host:port. A datagram is sent whole or not at all, so
// there is no partial-write loop here.
inline bool udp_send_to(int64_t id, const char* data, size_t n,
                        const std::string& host, int port, std::string* err) {
  detail::Sock* s = detail::get(id, Kind::Udp, err);
  if (!s) return false;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  detail::AddrInfoGuard res;
  std::string service = std::to_string(port);
  int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res.p);
  if (rc != 0) {
    if (err) *err = detail::gai_message(rc);
    return false;
  }
  long deadline = detail::deadline_from(s->timeout_ms);
  for (;;) {
#if defined(_WIN32)
    int w = ::sendto(s->fd, data, static_cast<int>(n), detail::kSendFlags,
                     res.p->ai_addr, static_cast<int>(res.p->ai_addrlen));
#else
    ssize_t w = ::sendto(s->fd, data, n, detail::kSendFlags, res.p->ai_addr,
                         static_cast<socklen_t>(res.p->ai_addrlen));
#endif
    if (w >= 0) return true;
    int e = detail::last_error();
    if (detail::is_eintr(e)) continue;
    if (detail::is_wouldblock(e)) {  // send buffer full: wait for room
      IoStatus st = detail::wait_ready(s->fd, /*want_write=*/true, deadline, err);
      if (st == IoStatus::Ok) continue;
      if (st == IoStatus::Timeout && err) *err = "send timed out";
      return false;
    }
    if (err) *err = detail::error_string(e);
    return false;
  }
}

// Receive one datagram (up to `max` bytes; the rest of an oversized datagram is
// discarded, as UDP dictates) and report its sender.
inline IoStatus udp_recv_from(int64_t id, size_t max, std::string& out,
                              std::string& host, int& port, std::string* err) {
  out.clear();
  host.clear();
  port = 0;
  detail::Sock* s = detail::get(id, Kind::Udp, err);
  if (!s) return IoStatus::Error;
  long deadline = detail::deadline_from(s->timeout_ms);
  std::string buf(max, '\0');
  for (;;) {
    IoStatus st = detail::wait_ready(s->fd, false, deadline, err);
    if (st != IoStatus::Ok) return st;
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
#if defined(_WIN32)
    int n = ::recvfrom(s->fd, buf.data(), static_cast<int>(max), 0,
                       reinterpret_cast<sockaddr*>(&ss), &len);
#else
    ssize_t n = ::recvfrom(s->fd, buf.data(), max, 0,
                           reinterpret_cast<sockaddr*>(&ss), &len);
#endif
    if (n < 0) {
      int e = detail::last_error();
      if (detail::is_eintr(e) || detail::is_wouldblock(e)) continue;
      if (err) *err = detail::error_string(e);
      return IoStatus::Error;
    }
    out.assign(buf.data(), static_cast<size_t>(n));
    detail::addr_to_host_port(reinterpret_cast<sockaddr*>(&ss), len, host, port);
    return IoStatus::Ok;
  }
}

}  // namespace culebra::net
