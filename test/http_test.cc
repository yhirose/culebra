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

  std::printf("\nhttp: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
