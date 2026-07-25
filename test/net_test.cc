// Deterministic, offline unit test for the value-neutral socket core
// (include/net.h). Everything runs over loopback on ephemeral ports in a single
// thread — a TCP connect completes into the listener's accept queue before
// accept() is called, so no threading is needed and nothing depends on external
// DNS or reachability.
//
// Covers what the three backends will drive: connect/listen/accept, the
// buffered stream reader (read / read_line / read_exact / EOF), write_all,
// half-close, per-handle timeouts, UDP round-trip, resolve, and the graceful
// error paths (closed handle, wrong kind, refused connect).
//
// Built and run by CTest (see CMakeLists.txt). No interpreter/JIT linkage.

#include <net.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace nt = culebra::net;

static int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

// Report the error text too — a failing syscall assertion is unreadable without
// it.
#define CHECK_OK(cond, err)                                                \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s (err: %s)\n", __FILE__,         \
                   __LINE__, #cond, (err).c_str());                        \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

// A connected loopback pair plus the listener that produced it.
struct Pair {
  int64_t listener = -1;
  int64_t client = -1;
  int64_t server = -1;
  int port = 0;
};

static bool make_pair(Pair& p, long timeout_ms = 2000) {
  std::string err;
  p.listener = nt::listen("127.0.0.1", 0, 0, &err);
  CHECK_OK(p.listener >= 0, err);
  if (p.listener < 0) return false;
  nt::set_timeout(p.listener, timeout_ms, &err);
  std::string host;
  CHECK_OK(nt::local_addr(p.listener, host, p.port, &err), err);
  CHECK(p.port > 0);
  p.client = nt::connect("127.0.0.1", p.port, timeout_ms, &err);
  CHECK_OK(p.client >= 0, err);
  if (p.client < 0) return false;
  CHECK_OK(nt::accept(p.listener, &p.server, &err) == nt::IoStatus::Ok, err);
  return p.server >= 0;
}

static void close_pair(Pair& p) {
  nt::close_handle(p.client);
  nt::close_handle(p.server);
  nt::close_handle(p.listener);
}

// Grab a port that is guaranteed to have no listener: bind one, read the port,
// then close it.
static int free_port() {
  std::string err, host;
  int port = 0;
  int64_t l = nt::listen("127.0.0.1", 0, 0, &err);
  if (l < 0) return 0;
  nt::local_addr(l, host, port, &err);
  nt::close_handle(l);
  return port;
}

static void test_roundtrip() {
  Pair p;
  if (!make_pair(p)) return;
  std::string err, out;

  CHECK_OK(nt::write_all(p.client, "hello", 5, &err) == nt::IoStatus::Ok, err);
  CHECK_OK(nt::read(p.server, 64, out, &err) == nt::IoStatus::Ok, err);
  CHECK(out == "hello");

  // The other direction, and a short read: ask for less than has arrived.
  CHECK_OK(nt::write_all(p.server, "abcdef", 6, &err) == nt::IoStatus::Ok, err);
  CHECK_OK(nt::read(p.client, 3, out, &err) == nt::IoStatus::Ok, err);
  CHECK(out == "abc");
  CHECK_OK(nt::read(p.client, 64, out, &err) == nt::IoStatus::Ok, err);
  CHECK(out == "def");

  close_pair(p);
}

static void test_read_line() {
  Pair p;
  if (!make_pair(p)) return;
  std::string err, out;

  // CRLF, LF, and a final unterminated line, all in one segment.
  const char* payload = "first\r\nsecond\nthird";
  CHECK_OK(nt::write_all(p.client, payload, std::strlen(payload), &err) ==
               nt::IoStatus::Ok,
           err);
  CHECK_OK(nt::shutdown_write(p.client, &err), err);

  CHECK_OK(nt::read_line(p.server, out, &err) == nt::IoStatus::Ok, err);
  CHECK(out == "first");  // \r stripped
  CHECK_OK(nt::read_line(p.server, out, &err) == nt::IoStatus::Ok, err);
  CHECK(out == "second");
  CHECK_OK(nt::read_line(p.server, out, &err) == nt::IoStatus::Ok, err);
  CHECK(out == "third");  // unterminated tail still yields a line
  CHECK(nt::read_line(p.server, out, &err) == nt::IoStatus::Eof);
  CHECK(out.empty());

  // An empty line is a line, not EOF.
  Pair q;
  if (make_pair(q)) {
    CHECK_OK(nt::write_all(q.client, "\n\n", 2, &err) == nt::IoStatus::Ok, err);
    CHECK_OK(nt::read_line(q.server, out, &err) == nt::IoStatus::Ok, err);
    CHECK(out.empty());
    CHECK_OK(nt::read_line(q.server, out, &err) == nt::IoStatus::Ok, err);
    CHECK(out.empty());
    close_pair(q);
  }

  close_pair(p);
}

static void test_read_exact_and_eof() {
  Pair p;
  if (!make_pair(p)) return;
  std::string err, out;

  CHECK_OK(nt::write_all(p.client, "0123456789", 10, &err) == nt::IoStatus::Ok,
           err);
  CHECK_OK(nt::read_exact(p.server, 4, out, &err) == nt::IoStatus::Ok, err);
  CHECK(out == "0123");
  CHECK_OK(nt::read_exact(p.server, 6, out, &err) == nt::IoStatus::Ok, err);
  CHECK(out == "456789");

  // Peer closes early: a short read_exact reports Eof and hands back the tail.
  CHECK_OK(nt::write_all(p.client, "xy", 2, &err) == nt::IoStatus::Ok, err);
  nt::close_handle(p.client);
  p.client = -1;
  CHECK(nt::read_exact(p.server, 8, out, &err) == nt::IoStatus::Eof);
  CHECK(out == "xy");

  // Once drained, a plain read reports Eof with no data.
  CHECK(nt::read(p.server, 64, out, &err) == nt::IoStatus::Eof);
  CHECK(out.empty());

  close_pair(p);
}

static void test_timeouts() {
  std::string err, out;

  // accept with nothing connecting.
  int64_t l = nt::listen("127.0.0.1", 0, 0, &err);
  CHECK_OK(l >= 0, err);
  nt::set_timeout(l, 120, &err);
  int64_t accepted = -1;
  CHECK(nt::accept(l, &accepted, &err) == nt::IoStatus::Timeout);
  nt::close_handle(l);

  // read with a silent peer.
  Pair p;
  if (make_pair(p)) {
    nt::set_timeout(p.server, 120, &err);
    CHECK(nt::read(p.server, 16, out, &err) == nt::IoStatus::Timeout);
    CHECK(nt::read_line(p.server, out, &err) == nt::IoStatus::Timeout);
    CHECK(nt::read_exact(p.server, 4, out, &err) == nt::IoStatus::Timeout);
    // A timeout leaves the socket usable: data sent afterwards still arrives.
    CHECK_OK(nt::write_all(p.client, "late", 4, &err) == nt::IoStatus::Ok, err);
    CHECK_OK(nt::read(p.server, 16, out, &err) == nt::IoStatus::Ok, err);
    CHECK(out == "late");
    close_pair(p);
  }
}

static void test_addresses() {
  Pair p;
  if (!make_pair(p)) return;
  std::string err, host;
  int port = 0;

  CHECK_OK(nt::peer_addr(p.server, host, port, &err), err);
  CHECK(host == "127.0.0.1");
  CHECK(port > 0);

  // The server's peer is the client's local end.
  std::string chost;
  int cport = 0;
  CHECK_OK(nt::local_addr(p.client, chost, cport, &err), err);
  CHECK(cport == port);

  // And the client's peer is the listening port.
  CHECK_OK(nt::peer_addr(p.client, host, port, &err), err);
  CHECK(port == p.port);

  CHECK_OK(nt::set_nodelay(p.client, true, &err), err);

  close_pair(p);
}

static void test_errors() {
  std::string err, out;

  // Refused connect: nothing is listening on a just-released port.
  int port = free_port();
  CHECK(port > 0);
  err.clear();
  CHECK(nt::connect("127.0.0.1", port, 2000, &err) < 0);
  CHECK(!err.empty());

  // Unresolvable host (RFC 2606 reserves .invalid).
  err.clear();
  CHECK(nt::connect("no-such-host.invalid", 80, 2000, &err) < 0);
  CHECK(!err.empty());

  // Closed handle: graceful error, never a dereference.
  Pair p;
  if (make_pair(p)) {
    int64_t sid = p.server;
    close_pair(p);
    err.clear();
    CHECK(nt::read(sid, 4, out, &err) == nt::IoStatus::Error);
    CHECK(err == "operation on a closed socket");
    CHECK(!nt::is_open(sid));
  }

  // Forged id.
  err.clear();
  CHECK(nt::read(999999, 4, out, &err) == nt::IoStatus::Error);
  CHECK(err == "operation on a closed socket");

  // Wrong kind: a listener is not a stream.
  int64_t l = nt::listen("127.0.0.1", 0, 0, &err);
  CHECK_OK(l >= 0, err);
  err.clear();
  CHECK(nt::read(l, 4, out, &err) == nt::IoStatus::Error);
  CHECK(err == "wrong socket kind for this operation");
  // ... and a stream is not a listener.
  Pair q;
  if (make_pair(q)) {
    int64_t accepted = -1;
    err.clear();
    CHECK(nt::accept(q.client, &accepted, &err) == nt::IoStatus::Error);
    CHECK(err == "wrong socket kind for this operation");
    close_pair(q);
  }
  nt::close_handle(l);

  // close is idempotent.
  nt::close_handle(l);
  nt::close_handle(-1);
}

static void test_udp() {
  std::string err, out, host;
  int port = 0;

  int64_t a = nt::udp_open("127.0.0.1", 0, &err);
  CHECK_OK(a >= 0, err);
  int64_t b = nt::udp_open("127.0.0.1", 0, &err);
  CHECK_OK(b >= 0, err);
  if (a < 0 || b < 0) return;

  std::string ahost, bhost;
  int aport = 0, bport = 0;
  CHECK_OK(nt::local_addr(a, ahost, aport, &err), err);
  CHECK_OK(nt::local_addr(b, bhost, bport, &err), err);

  CHECK_OK(nt::udp_send_to(a, "ping", 4, "127.0.0.1", bport, &err), err);
  nt::set_timeout(b, 2000, &err);
  CHECK_OK(nt::udp_recv_from(b, 1500, out, host, port, &err) == nt::IoStatus::Ok,
           err);
  CHECK(out == "ping");
  CHECK(host == "127.0.0.1");
  CHECK(port == aport);  // the sender's bound port

  // Reply to the reported sender.
  CHECK_OK(nt::udp_send_to(b, "pong", 4, host, port, &err), err);
  nt::set_timeout(a, 2000, &err);
  CHECK_OK(nt::udp_recv_from(a, 1500, out, host, port, &err) == nt::IoStatus::Ok,
           err);
  CHECK(out == "pong");

  // An empty datagram is data, not EOF (UDP has no EOF).
  CHECK_OK(nt::udp_send_to(a, "", 0, "127.0.0.1", bport, &err), err);
  CHECK_OK(nt::udp_recv_from(b, 1500, out, host, port, &err) == nt::IoStatus::Ok,
           err);
  CHECK(out.empty());

  // Oversized datagram is truncated to `max`, not an error.
  std::string big(2000, 'z');
  CHECK_OK(nt::udp_send_to(a, big.data(), big.size(), "127.0.0.1", bport, &err),
           err);
  CHECK_OK(nt::udp_recv_from(b, 100, out, host, port, &err) == nt::IoStatus::Ok,
           err);
  CHECK(out.size() == 100);

  // Timeout with no sender.
  nt::set_timeout(b, 120, &err);
  CHECK(nt::udp_recv_from(b, 1500, out, host, port, &err) ==
        nt::IoStatus::Timeout);

  CHECK_OK(nt::set_broadcast(a, true, &err), err);

  // A UDP handle is not a stream.
  err.clear();
  CHECK(nt::read(a, 4, out, &err) == nt::IoStatus::Error);
  CHECK(err == "wrong socket kind for this operation");

  nt::close_handle(a);
  nt::close_handle(b);
}

static void test_resolve() {
  std::string err;
  std::vector<std::string> addrs;

  // A numeric address always resolves to itself — no DNS involved, so this is
  // safe on a sandboxed CI runner.
  CHECK_OK(nt::resolve("127.0.0.1", addrs, &err), err);
  CHECK(addrs.size() == 1);
  CHECK(!addrs.empty() && addrs[0] == "127.0.0.1");

  CHECK_OK(nt::resolve("::1", addrs, &err), err);
  CHECK(!addrs.empty() && addrs[0] == "::1");

  err.clear();
  CHECK(!nt::resolve("no-such-host.invalid", addrs, &err));
  CHECK(!err.empty());
  CHECK(addrs.empty());
}

// A pending Ctrl+C must break a blocking wait with the cooperative Interrupted
// instead of stalling until the timeout — the property that makes one Ctrl+C
// stop a blocked read identically under interp, JIT and AOT.
static void test_interrupt() {
  Pair p;
  if (!make_pair(p)) return;
  std::string err, out;

  culebra::request_interrupt();
  bool threw = false;
  try {
    nt::read(p.server, 16, out, &err);  // silent peer: would block forever
  } catch (const culebra::CulebraError& e) {
    threw = true;
    CHECK(e.kind == "Interrupted");
  }
  CHECK(threw);

  // The flag is one-shot: the socket is usable again afterwards.
  CHECK_OK(nt::write_all(p.client, "after", 5, &err) == nt::IoStatus::Ok, err);
  CHECK_OK(nt::read(p.server, 16, out, &err) == nt::IoStatus::Ok, err);
  CHECK(out == "after");

  close_pair(p);
}

// Every failure path must release its descriptor. A refused connect and a
// closed round-trip, repeated well past the usual 1024-fd limit, would hit
// EMFILE if any path leaked.
static void test_no_fd_leak() {
  std::string err;
  int port = free_port();
  for (int i = 0; i < 600; i++) {
    err.clear();
    if (nt::connect("127.0.0.1", port, 2000, &err) >= 0) {
      CHECK(false);  // nothing is listening there
      return;
    }
  }
  for (int i = 0; i < 600; i++) {
    Pair p;
    if (!make_pair(p)) {
      std::fprintf(stderr, "  fd exhaustion at iteration %d\n", i);
      return;
    }
    close_pair(p);
  }
}

// Closing a handle must return its slot to the free list, so a long-running
// accept loop doesn't grow the table forever.
static void test_slot_reuse() {
  std::string err;
  int64_t first = nt::listen("127.0.0.1", 0, 0, &err);
  CHECK_OK(first >= 0, err);
  nt::close_handle(first);
  int64_t second = nt::listen("127.0.0.1", 0, 0, &err);
  CHECK_OK(second >= 0, err);
  CHECK(second == first);
  nt::close_handle(second);
}

int main() {
  test_roundtrip();
  test_read_line();
  test_read_exact_and_eof();
  test_timeouts();
  test_addresses();
  test_errors();
  test_udp();
  test_resolve();
  test_interrupt();
  test_no_fd_leak();
  test_slot_reuse();

  if (g_failures == 0) {
    std::puts("net_test: all checks passed");
    return 0;
  }
  std::fprintf(stderr, "net_test: %d check(s) failed\n", g_failures);
  return 1;
}
