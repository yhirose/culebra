//
//  http_test.cc
//
//  Self-contained test suite for include/http.h (the value-neutral HTTP
//  client core behind the `Http` stdlib namespace). Spins up an in-process
//  cpp-httplib server on an ephemeral loopback port so the happy paths are
//  deterministic and need no network. Also covers the transport-error and
//  URL-validation paths.
//
//  Build (via CMake/ctest): `ctest -R http_test`. Requires CULEBRA_ENABLE_HTTP
//  (OpenSSL); the server here is plain HTTP (no TLS) so the test stays offline.
//

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "http.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const char* expr, int line) {
  if (cond) {
    g_pass++;
  } else {
    g_fail++;
    std::printf("  FAIL (line %d): %s\n", line, expr);
  }
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

using culebra::http::http_request;
using culebra::http::HttpRequest;

}  // namespace

int main() {
  // encode_query: form-urlencoding shared by params: and form:.
  CHECK(culebra::http::encode_query({{"a", "b c"}, {"x", "1&2"}}) ==
        "a=b%20c&x=1%262");
  CHECK(culebra::http::encode_query({}) == "");

  // SseDecoder: parse a text/event-stream, including a comment, a typed event
  // with id, multi-line data, and an event split across two feed() calls (so
  // the line buffer carries a partial line). This is what Http.sse rides on.
  {
    std::vector<culebra::http::SseEvent> events;
    culebra::http::SseDecoder dec;
    dec.handler = [&events](const culebra::http::SseEvent& e) {
      events.push_back(e);
      return true;
    };
    auto feed = [&dec](const char* s) { return dec.feed(s, std::strlen(s)); };
    feed(": comment\ndata: hello\n\n");
    feed("event: chat\nid: 42\ndata: line1\nda");  // partial last line
    feed("ta: line2\n\n");
    CHECK(events.size() == 2);
    CHECK(events[0].type == "message");      // default when no event: field
    CHECK(events[0].data == "hello");
    CHECK(events[0].id.empty());
    CHECK(events[1].type == "chat");
    CHECK(events[1].id == "42");
    CHECK(events[1].data == "line1\nline2");  // data lines joined with '\n'
  }

  // SseDecoder: returning false from the handler stops dispatch (the stream is
  // aborted) — what a throwing on_event callback maps to.
  {
    int calls = 0;
    culebra::http::SseDecoder dec;
    dec.handler = [&calls](const culebra::http::SseEvent&) {
      ++calls;
      return false;  // abort after the first event
    };
    const char* stream = "data: a\n\ndata: b\n\n";
    bool ok = dec.feed(stream, std::strlen(stream));
    CHECK(!ok);          // feed reports the abort
    CHECK(calls == 1);   // second event never dispatched
  }

  httplib::Server svr;
  svr.Get("/hello", [](const httplib::Request&, httplib::Response& res) {
    res.set_header("X-Test", "yes");
    res.set_content("hello world", "text/plain");
  });
  svr.Post("/echo", [](const httplib::Request& req, httplib::Response& res) {
    res.set_content(req.body, req.get_header_value("Content-Type"));
  });
  svr.Delete("/gone", [](const httplib::Request&, httplib::Response& res) {
    res.status = 204;
  });
  svr.Get("/q", [](const httplib::Request& req, httplib::Response& res) {
    // echo the `q` query param back (so the client's params encoding +
    // the server's decoding round-trip is observable)
    res.set_content(req.get_param_value("q"), "text/plain");
  });
  svr.Post("/mp", [](const httplib::Request& req, httplib::Response& res) {
    // Echo a canonical summary of the parsed multipart parts. The server
    // de-chunks (for streamed uploads) and parses multipart/form-data itself,
    // so this observes the client's part framing + boundary end-to-end.
    std::string out;
    for (const auto& [k, f] : req.form.fields) {
      out += "field:" + f.name + "=" + f.content + ";";
    }
    for (const auto& [k, f] : req.form.files) {
      out += "file:" + f.name + "[" + f.filename + "," + f.content_type + "," +
             std::to_string(f.content.size()) + "];";
    }
    res.set_content(out, "text/plain");
  });
  svr.Get("/api/echo", [](const httplib::Request& req, httplib::Response& res) {
    // Echo the (joined) request path + two headers, so an Http.client's
    // base_url join and default-header merge are observable end-to-end.
    res.set_content(req.path + "|" + req.get_header_value("X-Auth") + "|" +
                        req.get_header_value("X-Extra"),
                    "text/plain");
  });
  // 404 for anything unmatched is the cpp-httplib default.

  int port = svr.bind_to_any_port("127.0.0.1");
  CHECK(port > 0);
  std::thread th([&] { svr.listen_after_bind(); });
  svr.wait_until_ready();

  const std::string base = "http://127.0.0.1:" + std::to_string(port);

  // GET 200 — body + custom header + ok flag.
  {
    HttpRequest req;
    req.url = base + "/hello";
    auto r = http_request(req);
    CHECK(r.ok);
    CHECK(r.status == 200);
    CHECK(r.reason == "OK");           // status reason phrase
    CHECK(r.body == "hello world");
    bool found = false;
    for (auto& [k, v] : r.headers) {
      if (k == "X-Test" && v == "yes") found = true;
    }
    CHECK(found);
  }

  // Streaming GET via body_sink — chunks delivered, res->body stays empty
  // (this is what Http.download / Http.stream ride on).
  {
    HttpRequest req;
    req.url = base + "/hello";
    std::string streamed;
    req.body_sink = [&streamed](const char* d, size_t n) {
      streamed.append(d, n);
      return true;
    };
    auto r = http_request(req);
    CHECK(r.ok);
    CHECK(r.status == 200);
    CHECK(r.body.empty());            // not buffered into the result
    CHECK(streamed == "hello world"); // delivered to the sink instead
  }

  // body_sink returning false aborts the transfer (no crash).
  {
    HttpRequest req;
    req.url = base + "/hello";
    int calls = 0;
    req.body_sink = [&calls](const char*, size_t) {
      ++calls;
      return false;  // abort immediately
    };
    auto r = http_request(req);
    CHECK(calls >= 1);
    (void)r;  // transport may report ok or a canceled error; must not crash
  }

  // POST echo — body round-trips, content_type applied.
  {
    HttpRequest req;
    req.method = "POST";
    req.url = base + "/echo";
    req.body = "{\"a\":1}";
    req.content_type = "application/json";
    auto r = http_request(req);
    CHECK(r.ok);
    CHECK(r.status == 200);
    CHECK(r.body == "{\"a\":1}");
  }

  // Streaming upload via body_source — chunks are sent (chunked transfer) and
  // the server reassembles them; /echo returns the whole body. This is what
  // Http.post(..., body: fn(){...}) rides on.
  {
    HttpRequest req;
    req.method = "POST";
    req.url = base + "/echo";
    req.content_type = "text/plain";
    const char* parts[] = {"hello ", "streamed ", "upload"};
    size_t i = 0;
    req.body_source = [&](std::string& out) {
      if (i >= 3) return false;          // end of stream
      out = parts[i++];
      return true;
    };
    auto r = http_request(req);
    CHECK(r.ok);
    CHECK(r.status == 200);
    CHECK(r.body == "hello streamed upload");
  }

  // Query params — percent-encoded by the client, decoded by the server.
  {
    HttpRequest req;
    req.url = base + "/q";
    req.params = {{"q", "a b&c"}, {"n", "5"}};
    auto r = http_request(req);
    CHECK(r.ok);
    CHECK(r.status == 200);
    CHECK(r.body == "a b&c");  // the `&` and space survived encoding intact
  }

  // Multipart, all parts in memory — a text field + a file part. Sent with a
  // known Content-Length (no streaming); the server parses it into fields/files.
  {
    HttpRequest req;
    req.method = "POST";
    req.url = base + "/mp";
    req.multipart.push_back({"title", "", "", "My report", nullptr});
    req.multipart.push_back(
        {"doc", "data.csv", "text/csv", "a,b,c\n1,2,3\n", nullptr});
    auto r = http_request(req);
    CHECK(r.ok);
    CHECK(r.status == 200);
    CHECK(r.body ==
          "field:title=My report;file:doc[data.csv,text/csv,12];");
  }

  // Multipart with a streamed part — a producer source yields the body in
  // chunks, so the whole request is sent chunked; the server de-chunks and
  // reassembles the part. This is what files: {stream: fn(){...}} rides on.
  {
    HttpRequest req;
    req.method = "POST";
    req.url = base + "/mp";
    const char* chunks[] = {"row0\n", "row1\n", "row2\n"};
    size_t i = 0;
    culebra::http::BodySource src = [&](std::string& out) {
      if (i >= 3) return false;
      out = chunks[i++];
      return true;
    };
    req.multipart.push_back({"f1", "", "", "field-value", nullptr});  // in-mem
    req.multipart.push_back({"report", "export.csv", "text/csv", "", src});
    auto r = http_request(req);
    CHECK(r.ok);
    CHECK(r.status == 200);
    // 15 bytes streamed (3 × "rowN\n"); the field rides alongside.
    CHECK(r.body ==
          "field:f1=field-value;file:report[export.csv,text/csv,15];");
  }

  // make_file_source — streams a file from disk in chunks, and reports a
  // missing file as nullptr (so the caller raises IOError, never crashes).
  {
    auto tmp = std::filesystem::temp_directory_path() /
               "culebra_http_mp_test.bin";
    std::string content(200000, 'X');  // larger than one 64 KiB chunk
    { std::ofstream(tmp, std::ios::binary) << content; }
    auto src = culebra::http::make_file_source(tmp.string());
    CHECK(static_cast<bool>(src));
    std::string got, chunk;
    while (src(chunk)) got += chunk;
    CHECK(got == content);
    std::filesystem::remove(tmp);
    CHECK(!culebra::http::make_file_source(
        "/no/such/dir/definitely_missing.bin"));
  }

  // DELETE 204 — completed round-trip, but not 2xx-with-body; ok is true
  // (204 is in [200,300)).
  {
    HttpRequest req;
    req.method = "DELETE";
    req.url = base + "/gone";
    auto r = http_request(req);
    CHECK(r.ok);
    CHECK(r.status == 204);
  }

  // 404 — a completed round-trip is ok at the transport level, status 404.
  {
    HttpRequest req;
    req.url = base + "/nope";
    auto r = http_request(req);
    CHECK(r.ok);          // transport succeeded
    CHECK(r.status == 404);
  }

  // Persistent client (Http.client) — base_url join, default-header merge,
  // connection reuse, and safe close. This is what the Http.client handle rides.
  {
    std::string err;
    int64_t cid = culebra::http::http_client_open(base + "/api",
                                                  {{"X-Auth", "k"}}, 0, true, err);
    CHECK(cid >= 0);
    CHECK(err.empty());
    // relative path joins under /api; the default X-Auth header rides along.
    HttpRequest r1;
    r1.method = "GET";
    r1.url = "/echo";
    auto a = culebra::http::http_client_request(cid, r1);
    CHECK(a.ok);
    CHECK(a.body == "/api/echo|k|");
    // a per-request header adds X-Extra and overrides the default X-Auth.
    HttpRequest r2;
    r2.method = "GET";
    r2.url = "/echo";
    r2.headers = {{"X-Extra", "e"}, {"X-Auth", "z"}};
    auto b = culebra::http::http_client_request(cid, r2);
    CHECK(b.ok);
    CHECK(b.body == "/api/echo|z|e");
    // reuse: a third request on the same handle still works (one connection).
    auto c = culebra::http::http_client_request(cid, r1);
    CHECK(c.ok && c.body == "/api/echo|k|");
    culebra::http::http_client_close(cid);
    // a request on a closed handle is an error result, never a crash.
    auto d = culebra::http::http_client_request(cid, r1);
    CHECK(!d.ok);
    CHECK(d.error.find("closed") != std::string::npos);
  }

  // Http.client with a bad base_url (no scheme) fails to open (-1 + err).
  {
    std::string err;
    int64_t cid = culebra::http::http_client_open("example.com", {}, 0, true, err);
    CHECK(cid < 0);
    CHECK(err.find("scheme") != std::string::npos);
  }

  svr.stop();
  th.join();

  // Transport failure — connection refused on a dead port. No response.
  {
    HttpRequest req;
    req.url = "http://127.0.0.1:1/x";
    req.timeout_sec = 2;
    auto r = http_request(req);
    CHECK(!r.ok);
    CHECK(r.status == 0);
    CHECK(!r.error.empty());
  }

  // URL validation — missing scheme is a failure, not a crash.
  {
    HttpRequest req;
    req.url = "example.com/x";
    auto r = http_request(req);
    CHECK(!r.ok);
    CHECK(r.error.find("scheme") != std::string::npos);
  }

  // --- WebSocket round-trip (client path through the WsConn registry) ---
  // Its own server (a WS handler occupies a connection for its lifetime), so it
  // stays independent of the shared server's thread pool.
  {
    httplib::Server wsvr;
    wsvr.WebSocket("/ws", [](const httplib::Request&,
                             httplib::ws::WebSocket& ws) {
      // Drive the WsConn registry exactly as the backend trampolines do.
      int64_t cid = culebra::http::ws_register_server(&ws);
      std::string m;
      while (culebra::http::ws_receive(cid, m) == 1) {
        culebra::http::ws_send(cid, m.data(), m.size());
      }
      culebra::http::ws_unregister(cid);
    });
    int wport = wsvr.bind_to_any_port("127.0.0.1");
    CHECK(wport > 0);
    std::thread wth([&] { wsvr.listen_after_bind(); });
    wsvr.wait_until_ready();

    std::string err;
    int64_t id = culebra::http::ws_client_open(
        "ws://127.0.0.1:" + std::to_string(wport) + "/ws", err);
    CHECK(id >= 0);
    CHECK(err.empty());
    CHECK(culebra::http::ws_is_open(id));
    CHECK(culebra::http::ws_send(id, "ping", 4));
    std::string got;
    CHECK(culebra::http::ws_receive(id, got) == 1);
    CHECK(got == "ping");  // server echoed it back
    CHECK(culebra::http::ws_send(id, "again", 5));
    CHECK(culebra::http::ws_receive(id, got) == 1);
    CHECK(got == "again");
    culebra::http::ws_close(id);
    culebra::http::ws_unregister(id);             // frees the client WsConn
    CHECK(!culebra::http::ws_send(id, "x", 1));   // stale id fails safely

    wsvr.stop();
    wth.join();
  }

  // A bad/unreachable URL fails to open (no crash).
  {
    std::string err;
    int64_t id = culebra::http::ws_client_open("ws://127.0.0.1:1/x", err);
    CHECK(id < 0);
    CHECK(!err.empty());
  }

  // WsConn registry slot+generation (no ABA): a reused slot bumps the
  // generation so a stale id stays dead.
  {
    culebra::http::WsConn a, b;
    int64_t ida = culebra::http::g_ws_conns.add(&a);
    CHECK(culebra::http::g_ws_conns.get(ida) == &a);
    culebra::http::g_ws_conns.invalidate(ida);
    CHECK(culebra::http::g_ws_conns.get(ida) == nullptr);  // stale after invalidate
    int64_t idb = culebra::http::g_ws_conns.add(&b);        // reuses the slot
    CHECK(idb != ida);                                      // generation bumped
    CHECK(culebra::http::g_ws_conns.get(ida) == nullptr);  // old id still dead
    CHECK(culebra::http::g_ws_conns.get(idb) == &b);
    culebra::http::g_ws_conns.invalidate(idb);
  }

  // --- Http server core (registry + route/static/close contract) ---
  // The listen()/serve path is exercised end-to-end across all three backends
  // by tests/test_http_server.cul (isolate + loopback); here we cover the
  // value-neutral, non-blocking core deterministically: the id registry, route
  // method validation, static mount errors, and idempotent close.
  {
    using culebra::http::RouteHandler;
    RouteHandler noop = [](const httplib::Request&, httplib::Response&) {};

    int64_t sid = culebra::http::http_server_open();
    CHECK(sid >= 0);

    std::string err;
    culebra::http::http_server_route(sid, "GET", "/x", noop, err);
    CHECK(err.empty());                 // a supported method registers cleanly
    err.clear();
    culebra::http::http_server_route(sid, "POST", "/y", noop, err);
    CHECK(err.empty());
    err.clear();
    culebra::http::http_server_route(sid, "BREW", "/z", noop, err);
    CHECK(err.find("unsupported method") != std::string::npos);

    // static: a real dir mounts; a missing dir reports an error (no crash).
    err.clear();
    culebra::http::http_server_static(sid, "/s",
                                      std::filesystem::temp_directory_path()
                                          .string(),
                                      err);
    CHECK(err.empty());
    err.clear();
    culebra::http::http_server_static(sid, "/s2",
                                      "/no/such/dir/xyzzy12345", err);
    CHECK(!err.empty());

    // A forged/closed id resolves to nullptr and fails safely.
    err.clear();
    culebra::http::http_server_route(99999, "GET", "/x", noop, err);
    CHECK(err.find("closed") != std::string::npos);

    culebra::http::http_server_close(sid);
    err.clear();
    culebra::http::http_server_route(sid, "GET", "/x", noop, err);
    CHECK(err.find("closed") != std::string::npos);  // closed id no longer routes
    culebra::http::http_server_close(sid);            // idempotent: no crash
  }

  // --- Streaming sink registry (slot + generation defense) ---
  {
    std::string buf;
    httplib::DataSink sink;
    sink.write = [&buf](const char* d, size_t n) {
      buf.append(d, n);
      return true;
    };
    int64_t id = culebra::http::g_http_sinks.add(&sink);
    CHECK(culebra::http::http_sink_write(id, "ab", 2));   // live sink writes
    CHECK(culebra::http::http_sink_write(id, "c", 1));
    CHECK(buf == "abc");
    culebra::http::g_http_sinks.invalidate(id);
    CHECK(!culebra::http::http_sink_write(id, "z", 1));    // stale id fails safely
    CHECK(buf == "abc");                                   // nothing more written

    // Reusing the slot bumps the generation, so the old id stays invalid (no
    // ABA: a captured sink can never write to a later connection's stream).
    std::string buf2;
    httplib::DataSink sink2;
    sink2.write = [&buf2](const char* d, size_t n) {
      buf2.append(d, n);
      return true;
    };
    int64_t id2 = culebra::http::g_http_sinks.add(&sink2);
    CHECK(id2 != id);                                      // distinct id
    CHECK(!culebra::http::http_sink_write(id, "z", 1));    // old id still dead
    CHECK(culebra::http::http_sink_write(id2, "Q", 1));    // new id is live
    CHECK(buf2 == "Q");
    culebra::http::g_http_sinks.invalidate(id2);
  }

  // A handle that bound and never served still owns its listening socket, and
  // close must release it. cpp-httplib gated Server::stop()'s socket teardown on
  // is_running_, so this leaked one fd (and held the port) per bind — reachable
  // only since bind became a call of its own. Linux-only: it needs /proc to
  // count descriptors, and the fix is platform-independent.
#ifdef __linux__
  {
    auto open_fds = [] {
      size_t n = 0;
      for (const auto& e :
           std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)e;
        n++;
      }
      return n;
    };
    size_t before = open_fds();
    for (int i = 0; i < 20; i++) {
      int64_t id = culebra::http::http_server_open();
      std::string err;
      CHECK(culebra::http::http_server_bind(id, "127.0.0.1", 0, err) > 0);
      culebra::http::http_server_close(id);
    }
    CHECK(open_fds() == before);
  }
#endif

  std::printf("\nhttp: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
