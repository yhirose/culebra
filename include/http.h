#pragma once

// Type-neutral HTTP client core for the Http namespace.
//
// No dependency on culebra Value / JitValue / GC: both the interp and JIT
// backends call http_request() and adapt HttpResult into their own object
// representation. Keeping this header value-neutral lets stdlib_interp.h and
// stdlib_jit.h include it without pulling each other in (mirrors proc.h).
//
//   http_request — one request, blocking. Builds a cpp-httplib Client from the
//                  URL origin and sends a single Request; a transport failure
//                  (DNS/connect/TLS/timeout) is reported in `error`, an HTTP
//                  response (any status) sets `ok` and `status`.
//
// TLS is provided by cpp-httplib's OpenSSL path (CPPHTTPLIB_OPENSSL_SUPPORT,
// set by the build). The same code path serves a future BoringSSL swap — that
// is purely a link-time decision and does not touch this file.

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <httplib.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <shared.h>  // throw_if_interrupted / culebra_g_sigint (Ctrl+C wiring)
#include <vfs.h>     // Dir / DiskDir / EmbeddedDir / serve_static (static assets)

namespace culebra::http {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// Per-chunk sink for streaming the response body. Return false to abort the
// transfer. When set on a request, the body is delivered to this sink as it
// arrives and HttpResult::body stays empty (no whole-body buffering). Used by
// `Http.download` (sink → file) and `Http.get(..., on_chunk:)` (sink → closure).
using BodySink = std::function<bool(const char* data, size_t len)>;

// Per-chunk source for streaming the request body (upload). Fill `out` with the
// next chunk and return true; return false at end-of-stream. When set, the body
// is sent chunked (Transfer-Encoding: chunked) instead of `body` whole — so a
// large upload never has to live in memory at once.
using BodySource = std::function<bool(std::string& out)>;

// One server-sent event (text/event-stream).
struct SseEvent {
  std::string type = "message";  // the `event:` field, default "message".
  std::string data;              // `data:` lines joined with '\n'.
  std::string id;                // the last `id:` field, if any.
};
// Return false from the handler to stop the stream.
using SseHandler = std::function<bool(const SseEvent&)>;

// One part of a multipart/form-data upload. `name` is the field name. A part is
// a text field when `filename` is empty and a file part when it is set. The body
// comes from `content` (held in memory) unless `source` is set, in which case
// the part is streamed chunk-by-chunk and `content` is ignored — so a large file
// or a slow-to-produce part never has to live in memory all at once. When any
// part streams, the whole body is sent chunked; otherwise it is sent with a
// known Content-Length.
struct MultipartPart {
  std::string name;
  std::string filename;      // empty → text field; non-empty → file part.
  std::string content_type;  // empty → no per-part Content-Type header.
  std::string content;       // in-memory body (used when source is null).
  BodySource source;         // set → stream this part's body.
};

// Build a BodySource that streams `path` in fixed-size chunks. Returns nullptr
// when the file cannot be opened (the caller reports the error). Value-neutral
// (pure std::ifstream), so both backends share one file-streaming path.
inline BodySource make_file_source(const std::string& path) {
  auto f = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (!*f) return nullptr;
  return [f](std::string& out) -> bool {
    if (!f->good()) return false;
    char buf[64 * 1024];
    f->read(buf, sizeof(buf));
    auto n = f->gcount();
    if (n <= 0) return false;  // EOF with nothing read.
    out.assign(buf, static_cast<size_t>(n));
    return true;
  };
}

// Pull the next non-empty chunk from `src` into `out`, skipping empty chunks so
// a chunked writer always makes progress. Returns false at end-of-stream.
// Shared by the streaming body_source and the streaming multipart parts.
inline bool next_chunk(const BodySource& src, std::string& out) {
  while (src(out)) {
    if (!out.empty()) return true;
  }
  return false;
}

// Incremental SSE parser (WHATWG event-stream): feed response-body chunks via
// feed(); on each complete event (terminated by a blank line) it calls
// `handler`. Use it as a BodySink: `req.body_sink = [&d](p,n){ return d.feed(p,n); }`.
struct SseDecoder {
  SseHandler handler;
  std::string buf_;   // bytes not yet split into a complete line.
  std::string ev_type_, ev_data_, ev_id_;
  bool have_field_ = false;

  bool feed(const char* data, size_t n) {
    buf_.append(data, n);
    size_t start = 0;
    for (;;) {
      size_t nl = buf_.find('\n', start);
      if (nl == std::string::npos) break;
      std::string line = buf_.substr(start, nl - start);
      start = nl + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF
      if (line.empty()) {  // blank line → dispatch the buffered event
        if (have_field_) {
          SseEvent e{ev_type_.empty() ? "message" : ev_type_, ev_data_, ev_id_};
          if (!handler(e)) return false;
        }
        ev_type_.clear();
        ev_data_.clear();
        ev_id_.clear();
        have_field_ = false;
        continue;
      }
      if (line[0] == ':') continue;  // comment
      size_t colon = line.find(':');
      std::string field = line.substr(0, colon);
      std::string value = colon == std::string::npos ? "" : line.substr(colon + 1);
      if (!value.empty() && value[0] == ' ') value.erase(0, 1);  // one space
      if (field == "event") {
        ev_type_ = value;
        have_field_ = true;
      } else if (field == "data") {
        if (!ev_data_.empty()) ev_data_ += '\n';
        ev_data_ += value;
        have_field_ = true;
      } else if (field == "id") {
        ev_id_ = value;
        have_field_ = true;
      }  // `retry:` and unknown fields are ignored.
    }
    buf_.erase(0, start);
    return true;
  }
};

struct HttpRequest {
  std::string method = "GET";      // "GET" / "POST" / "PUT" / "DELETE" / ...
  std::string url;                 // full URL including scheme (http/https).
  HeaderList params;               // query params, appended to the URL
                                   // (percent-encoded) as ?k=v&...
  HeaderList headers;              // request headers (verbatim).
  std::string body;                // request body (raw bytes); ignored if
                                   // body_source is set.
  std::string content_type;        // applied iff body/body_source set and no
                                   // explicit Content-Type header.
  long timeout_sec = 0;            // per-phase timeout; 0 => library default.
  bool follow_redirects = true;    // 3xx Location chasing.
  BodySink body_sink = nullptr;    // set → stream the response body (no buffer).
  BodySource body_source = nullptr;// set → stream the request body (chunked).
  std::vector<MultipartPart> multipart;  // non-empty → multipart/form-data body.
};

struct HttpResult {
  bool ok = false;        // transport succeeded and a response was received.
  long status = 0;        // HTTP status code (0 when ok is false).
  std::string reason;     // status reason phrase ("OK", "Not Found", …).
  std::string body;       // response body (raw bytes).
  HeaderList headers;     // response headers (insertion order from server).
  std::string error;      // transport error text (empty when ok).
};

inline bool _iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Split "scheme://host:port/path?query#frag" into the origin the Client ctor
// wants ("scheme://host:port") and the path+query to request ("/..."). A URL
// with no path component yields "/". Returns false (with `err` set) when the
// scheme separator is missing.
inline bool split_url(const std::string& url, std::string& origin,
                      std::string& path, std::string& err) {
  auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    err = "Http: URL must include a scheme (http:// or https://): " + url;
    return false;
  }
  auto host_start = scheme_end + 3;
  auto path_start = url.find('/', host_start);
  if (path_start == std::string::npos) {
    origin = url;
    path = "/";
  } else {
    origin = url.substr(0, path_start);
    path = url.substr(path_start);
  }
  if (host_start >= url.size() || url[host_start] == '/') {
    err = "Http: URL has no host: " + url;
    return false;
  }
  return true;
}

// Percent-encode name/value pairs as `k=v&k2=v2` (application/x-www-form-
// urlencoded form). Shared by query params (`?`+this) and `form:` bodies.
inline std::string encode_query(const HeaderList& pairs) {
  std::string q;
  for (const auto& [k, v] : pairs) {
    if (!q.empty()) q += "&";
    q += httplib::encode_uri_component(k) + "=" + httplib::encode_uri_component(v);
  }
  return q;
}

// http_request and the Http.client functions are the single OpenSSL/zlib choke:
// httplib::Client (the only code that pulls in TLS + gzip) is instantiated only
// inside the bodies below. Their linkage is partitioned across the AOT runtime
// archives exactly like tensor_eval_node:
//   - core archive   (CULEBRA_RT_HTTP_REQUEST_WEAK):   weak stubs, never
//     touch httplib::Client, so the archive references no ssl/zlib symbol
//     (merely including httplib.h pulls none — verified).
//   - http archive   (CULEBRA_RT_HTTP_REQUEST_STRONG): strong real bodies,
//     force-loaded only when the program uses Http (overrides the stubs).
//   - header-only / in-process JIT (neither): the normal inline bodies.
#if defined(CULEBRA_RT_HTTP_REQUEST_STRONG)
#define CULEBRA_RT_HTTP_LINKAGE
#elif defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
#define CULEBRA_RT_HTTP_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_HTTP_LINKAGE inline
#endif

// A persistent client holding one reused keep-alive connection plus a base URL
// and default headers (the Http.client handle's native backing). Opaque outside
// the gated real path below — the WEAK core archive never sees httplib::Client.
struct HttpClient;

// id → HttpClient* registry. Scripts hold a small integer id, never a raw
// pointer (matching the SQLite handle model — an invalid/forged id resolves to
// nullptr and fails safely instead of dereferencing arbitrary memory).
// thread_local because a client is non-sendable (one connection, one thread).
// Stores opaque pointers only, so it pulls in no httplib/TLS symbol and stays
// ungated; only open/close/request (which touch httplib) are gated.
inline thread_local std::vector<HttpClient*> g_http_clients;
inline thread_local std::vector<int64_t> g_http_client_free;
inline int64_t _http_client_register(HttpClient* c) {
  if (!g_http_client_free.empty()) {
    int64_t id = g_http_client_free.back();
    g_http_client_free.pop_back();
    g_http_clients[id] = c;
    return id;
  }
  g_http_clients.push_back(c);
  return static_cast<int64_t>(g_http_clients.size()) - 1;
}
inline HttpClient* _http_client_get(int64_t id) {
  if (id < 0 || id >= static_cast<int64_t>(g_http_clients.size())) return nullptr;
  return g_http_clients[id];
}
inline void _http_client_unregister(int64_t id) {
  if (id >= 0 && id < static_cast<int64_t>(g_http_clients.size()) &&
      g_http_clients[id]) {
    g_http_clients[id] = nullptr;
    g_http_client_free.push_back(id);
  }
}

// The httplib-touching helpers and HttpClient definition live behind the gate
// (referenced only by the real, non-stub function bodies), so the WEAK core
// archive pulls in no TLS/zlib symbol.
#if !defined(CULEBRA_RT_HTTP_REQUEST_WEAK)

// Merge `over` onto `base` with per-key (case-insensitive) override: a header
// present in both takes `over`'s value; others are appended. Used to layer a
// request's headers over an Http.client's defaults.
inline HeaderList merge_headers(const HeaderList& base, const HeaderList& over) {
  HeaderList out = base;
  for (const auto& [k, v] : over) {
    bool replaced = false;
    for (auto& [bk, bv] : out) {
      if (_iequals(bk, k)) { bv = v; replaced = true; break; }
    }
    if (!replaced) out.emplace_back(k, v);
  }
  return out;
}

// Join a client base path ("" or "/v1", no trailing slash) with a request path.
inline std::string _http_join_path(const std::string& base_path,
                                   const std::string& rel) {
  if (rel.empty()) return base_path.empty() ? "/" : base_path;
  if (rel[0] == '/') return base_path + rel;
  return base_path + "/" + rel;
}

// Build the httplib::Request (method, path, headers, body/multipart/streaming
// source, response sink) from `req` and the already-resolved `headers`/`path`.
// Shared by the one-off http_request and the persistent http_client_request so
// all body logic (params already folded into `path`) lives in one place. The
// body_sink lambda captures `req` by reference — the caller keeps `req` alive
// across the send.
inline void _http_build_request(const HttpRequest& req, const std::string& path,
                                const HeaderList& headers,
                                httplib::Request& hreq) {
  hreq.method = req.method;
  hreq.path = path;
  bool has_ct = false;
  for (const auto& [k, v] : headers) {
    hreq.headers.emplace(k, v);
    if (_iequals(k, "Content-Type")) has_ct = true;
  }
  if (!req.multipart.empty()) {
    // multipart/form-data. httplib's detail serializers give us correct
    // boundary generation and RFC-7578 part framing; we drive them ourselves
    // (rather than Client::Post(items)) so the body still flows through the
    // single send(Request) choke and composes with params/headers/into.
    std::string boundary = httplib::detail::make_multipart_data_boundary();
    if (!has_ct) {
      hreq.headers.emplace(
          "Content-Type",
          httplib::detail::serialize_multipart_formdata_get_content_type(boundary));
    }
    bool any_stream = false;
    for (const auto& p : req.multipart) {
      if (p.source) { any_stream = true; break; }
    }
    if (!any_stream) {
      // All parts in memory → one known-length body (best server compat).
      httplib::UploadFormDataItems items;
      items.reserve(req.multipart.size());
      for (const auto& p : req.multipart) {
        items.push_back({p.name, p.content, p.filename, p.content_type});
      }
      hreq.body = httplib::detail::serialize_multipart_formdata(items, boundary);
    } else {
      // Some part streams → emit the whole body chunked, interleaving each
      // part's header, body (in-memory content or pulled from its source), and
      // trailing CRLF, then the closing boundary. State lives in shared_ptrs so
      // the provider (a copyable std::function) keeps its cursor across calls.
      struct SPart {
        std::string header;   // serialized item-begin (owns its bytes).
        std::string content;  // in-memory body (empty when source set).
        BodySource source;
      };
      auto parts = std::make_shared<std::vector<SPart>>();
      parts->reserve(req.multipart.size());
      for (const auto& p : req.multipart) {
        httplib::UploadFormData meta{p.name, std::string(), p.filename,
                                     p.content_type};
        parts->push_back(
            {httplib::detail::serialize_multipart_formdata_item_begin(meta,
                                                                      boundary),
             p.content, p.source});
      }
      hreq.headers.emplace("Transfer-Encoding", "chunked");
      hreq.is_chunked_content_provider_ = true;
      auto idx = std::make_shared<size_t>(0);
      auto in_body = std::make_shared<bool>(false);  // false → emit header next.
      hreq.content_provider_ =
          [parts, boundary, idx, in_body](size_t, size_t,
                                          httplib::DataSink& sink) -> bool {
        static const char crlf[] = "\r\n";
        if (*idx >= parts->size()) {
          std::string fin =
              httplib::detail::serialize_multipart_formdata_finish(boundary);
          sink.write(fin.data(), fin.size());
          sink.done();
          return true;
        }
        SPart& sp = (*parts)[*idx];
        if (!*in_body) {
          sink.write(sp.header.data(), sp.header.size());
          *in_body = true;
          return true;
        }
        if (sp.source) {
          std::string chunk;
          if (next_chunk(sp.source, chunk)) {
            sink.write(chunk.data(), chunk.size());
            return true;
          }
          // EOF for this part (a producer error is rethrown after send).
          sink.write(crlf, 2);
          ++*idx;
          *in_body = false;
          return true;
        }
        if (!sp.content.empty()) sink.write(sp.content.data(), sp.content.size());
        sink.write(crlf, 2);
        ++*idx;
        *in_body = false;
        return true;
      };
    }
  } else if (req.body_source) {
    // Streaming upload: send the body chunked, pulling from the source. The
    // provider writes one non-empty chunk per call (skipping empties so the
    // chunked writer always makes progress) and calls sink.done() at EOF.
    if (!has_ct && !req.content_type.empty()) {
      hreq.headers.emplace("Content-Type", req.content_type);
    }
    // Announce chunked framing: send() writes chunked bytes from the provider
    // but does not set this header itself (Post(ContentProvider) normally
    // would), so the server wouldn't know to de-chunk the body.
    hreq.headers.emplace("Transfer-Encoding", "chunked");
    hreq.is_chunked_content_provider_ = true;
    hreq.content_provider_ = [src = req.body_source](
                                 size_t, size_t, httplib::DataSink& sink) -> bool {
      std::string chunk;
      if (next_chunk(src, chunk)) {
        sink.write(chunk.data(), chunk.size());
      } else {
        sink.done();
      }
      return true;
    };
  } else if (!req.body.empty()) {
    hreq.body = req.body;
    if (!has_ct && !req.content_type.empty()) {
      hreq.headers.emplace("Content-Type", req.content_type);
    }
  }
  // Streaming: route the response body to the sink as it arrives instead of
  // buffering it into res->body (which then stays empty).
  if (req.body_sink) {
    hreq.content_receiver = [&req](const char* data, size_t len, uint64_t,
                                   uint64_t) { return req.body_sink(data, len); };
  }
}

// Send `hreq` over `cli` and map the result into an HttpResult, with cooperative
// Ctrl+C / isolate-cancel. send() blocks deep in the socket layer (connect,
// response-header wait, body) — not a runtime safepoint, and the internal
// poll/select swallow EINTR, so a single SIGINT couldn't break a hung request
// (only the second, force-killing press). A watcher thread polls the interrupt
// flag and, when it fires, calls cli.stop() — cpp-httplib's documented
// thread-safe way to shut down an in-flight socket so the blocked send() errors
// out. send() itself stays on THIS thread so the streaming callbacks
// (body_sink/body_source, which call back into culebra) keep running on the
// thread that owns the interpreter/JIT state. The flag is read, never consumed;
// after send() returns, throw_if_interrupted() honors a pending interrupt with
// the same cooperative Interrupted the loop safepoint raises. The watcher checks
// the process SIGINT flag and this thread's isolate-cancel flag (captured now —
// the watcher runs on a different thread, so its own current_runtime() would be
// the wrong one).
inline HttpResult _http_send(httplib::Client& cli, httplib::Request& hreq) {
  HttpResult out;
  auto* isolate_flag = current_runtime().interrupt_flag;
  std::mutex watch_m;
  std::condition_variable watch_cv;
  bool watch_done = false;
  std::thread watcher([&] {
    auto fired = [&] {
      if (culebra_g_sigint.load(std::memory_order_relaxed)) return true;
      return isolate_flag && isolate_flag != &culebra_g_sigint &&
             isolate_flag->load(std::memory_order_relaxed);
    };
    std::unique_lock<std::mutex> lk(watch_m);
    while (!watch_done) {
      watch_cv.wait_for(lk, std::chrono::milliseconds(50));
      if (watch_done) break;
      // Keep calling stop() (not just once) so we still catch the socket if the
      // interrupt fires before it opens — e.g. mid DNS/connect.
      if (fired()) cli.stop();
    }
  });

  auto res = cli.send(hreq);

  {
    std::lock_guard<std::mutex> lk(watch_m);
    watch_done = true;
  }
  watch_cv.notify_all();
  watcher.join();
  throw_if_interrupted();  // pending Ctrl+C / cancel → cooperative Interrupted

  if (!res) {
    out.error = "Http: " + httplib::to_string(res.error());
    return out;
  }
  out.ok = true;
  out.status = res->status;
  out.reason = res->reason;
  out.body = std::move(res->body);
  out.headers.reserve(res->headers.size());
  for (const auto& [k, v] : res->headers) {
    out.headers.emplace_back(k, v);
  }
  return out;
}

struct HttpClient {
  httplib::Client cli;          // reused keep-alive connection.
  std::string base_path;        // leading path prefix ("" or "/v1"), no trailing
                                // slash; the origin lives in `cli`.
  HeaderList default_headers;   // layered under each request's headers.
  long timeout_sec = 0;         // carried so an absolute-URL one-off can reuse it.
  bool follow_redirects = true;
  std::mutex m;                 // one connection → serialize requests.
  explicit HttpClient(const std::string& origin) : cli(origin) {}
};

#endif  // !CULEBRA_RT_HTTP_REQUEST_WEAK

CULEBRA_RT_HTTP_LINKAGE HttpResult http_request(const HttpRequest& req) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  // Core archive stub: the real httplib::Client path is force-loaded from
  // culebra_rt_http only when the program uses Http, so a binary built
  // without Http never reaches httplib. Unreachable in practice (the Http
  // namespace is never called there); report gracefully rather than abort.
  (void)req;
  HttpResult stub;
  stub.error = "Http runtime not linked (no Http use detected at build)";
  return stub;
#else
  HttpResult out;
  std::string origin, path, err;
  if (!split_url(req.url, origin, path, err)) {
    out.error = std::move(err);
    return out;
  }
  // Append query params (percent-encoded), preserving any query already in url.
  if (!req.params.empty()) {
    path += (path.find('?') == std::string::npos ? "?" : "&") +
            encode_query(req.params);
  }

  httplib::Client cli(origin);
  if (!cli.is_valid()) {
    out.error = "Http: invalid URL or unsupported scheme: " + req.url;
    return out;
  }
  cli.set_follow_location(req.follow_redirects);
  // Tolerate servers that don't speak keep-alive cleanly on a one-shot call.
  cli.set_keep_alive(false);
  if (req.timeout_sec > 0) {
    cli.set_connection_timeout(req.timeout_sec, 0);
    cli.set_read_timeout(req.timeout_sec, 0);
    cli.set_write_timeout(req.timeout_sec, 0);
  }

  httplib::Request hreq;
  _http_build_request(req, path, req.headers, hreq);
  return _http_send(cli, hreq);
#endif  // CULEBRA_RT_HTTP_REQUEST_WEAK
}

// Open a persistent client bound to `base_url`'s origin, reusing one keep-alive
// connection. `default_headers` layer under each request; `timeout_sec` /
// `follow_redirects` are connection-level defaults. Returns a handle id (>= 0),
// or -1 (with `err` set) on a bad base_url. Caller closes it (http_client_close).
CULEBRA_RT_HTTP_LINKAGE int64_t http_client_open(const std::string& base_url,
                                                 HeaderList default_headers,
                                                 long timeout_sec,
                                                 bool follow_redirects,
                                                 std::string& err) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)base_url;
  (void)default_headers;
  (void)timeout_sec;
  (void)follow_redirects;
  err = "Http runtime not linked (no Http use detected at build)";
  return -1;
#else
  std::string origin, base_path, serr;
  if (!split_url(base_url, origin, base_path, serr)) {
    err = std::move(serr);
    return -1;
  }
  // "/" means no prefix; otherwise drop a trailing slash so join_path is clean.
  if (base_path == "/") base_path.clear();
  else if (!base_path.empty() && base_path.back() == '/') base_path.pop_back();
  auto* c = new HttpClient(origin);
  if (!c->cli.is_valid()) {
    delete c;
    err = "Http: invalid base_url or unsupported scheme: " + base_url;
    return -1;
  }
  c->base_path = std::move(base_path);
  c->default_headers = std::move(default_headers);
  c->timeout_sec = timeout_sec;
  c->follow_redirects = follow_redirects;
  c->cli.set_follow_location(follow_redirects);
  c->cli.set_keep_alive(true);  // reuse the connection across requests.
  if (timeout_sec > 0) {
    c->cli.set_connection_timeout(timeout_sec, 0);
    c->cli.set_read_timeout(timeout_sec, 0);
    c->cli.set_write_timeout(timeout_sec, 0);
  }
  return _http_client_register(c);
#endif
}

// Run `req` against the persistent client `id`. A relative `req.url` is joined to
// the client's base path and sent over the reused connection (serialized by the
// client mutex); an absolute `req.url` (with a scheme) can't reuse a connection
// bound to a different origin, so it goes through a one-off http_request. Either
// way the client's default headers layer under the request's. A closed/invalid
// id is an error result (never a crash).
CULEBRA_RT_HTTP_LINKAGE HttpResult http_client_request(int64_t id,
                                                       const HttpRequest& req) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id;
  (void)req;
  HttpResult stub;
  stub.error = "Http runtime not linked (no Http use detected at build)";
  return stub;
#else
  HttpClient* c = _http_client_get(id);
  if (!c) {
    HttpResult out;
    out.error = "Http: client is closed";
    return out;
  }
  if (req.url.find("://") != std::string::npos) {
    HttpRequest fresh = req;  // std::function members are copyable.
    fresh.headers = merge_headers(c->default_headers, req.headers);
    fresh.timeout_sec = c->timeout_sec;
    fresh.follow_redirects = c->follow_redirects;
    return http_request(fresh);
  }
  std::string path = _http_join_path(c->base_path, req.url);
  if (!req.params.empty()) {
    path += (path.find('?') == std::string::npos ? "?" : "&") +
            encode_query(req.params);
  }
  HeaderList headers = merge_headers(c->default_headers, req.headers);
  httplib::Request hreq;
  _http_build_request(req, path, headers, hreq);
  std::lock_guard<std::mutex> lk(c->m);
  return _http_send(c->cli, hreq);
#endif
}

// Close and free a persistent client (idempotent; a closed/invalid id no-ops).
CULEBRA_RT_HTTP_LINKAGE void http_client_close(int64_t id) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id;
#else
  HttpClient* c = _http_client_get(id);
  if (!c) return;
  _http_client_unregister(id);
  delete c;
#endif
}

// ============================== HTTP server ================================
//
// The inverse of the client: httplib::Server accepts connections and, for each
// matched route, calls back into a culebra handler closure. Invoking the
// closure is backend-specific (interp call_closure / JIT _culebra_invoke1), so
// this core stays value-neutral — the backend hands us a RouteHandler that
// reads the httplib request and fills the response. Handlers always run on a
// CulebraWorkerPool (one culebra runtime per worker thread; a value/GC is
// thread_local and non-sendable), never on the accept loop — so a slow handler
// can't block accepting new connections, and handlers must be Sendable.

// Backend-supplied per-route handler: read the request, fill the response. This
// is exactly httplib's Handler type; declaring it pulls no TLS symbol (only
// instantiating httplib::Server / httplib::Client does), so it stays ungated.
using RouteHandler =
    std::function<void(const httplib::Request&, httplib::Response&)>;

// Backend-supplied WebSocket handler: run a long-lived loop over `ws` (read /
// send) for one connection. httplib calls it on the worker thread, so it
// occupies that worker until it returns. Like RouteHandler, declaring it (ws is
// an incomplete type here) pulls no TLS symbol.
using WsHandler =
    std::function<void(const httplib::Request&, httplib::ws::WebSocket&)>;

// id → HttpServer* registry, same opaque-pointer model as the client: scripts
// hold a small integer id, a forged/closed id resolves to nullptr and fails
// safely. thread_local because a server (v0) runs on the thread that created it.
struct HttpServer;
inline thread_local std::vector<HttpServer*> g_http_servers;
inline thread_local std::vector<int64_t> g_http_server_free;
inline int64_t _http_server_register(HttpServer* s) {
  if (!g_http_server_free.empty()) {
    int64_t id = g_http_server_free.back();
    g_http_server_free.pop_back();
    g_http_servers[id] = s;
    return id;
  }
  g_http_servers.push_back(s);
  return static_cast<int64_t>(g_http_servers.size()) - 1;
}
inline HttpServer* _http_server_get(int64_t id) {
  if (id < 0 || id >= static_cast<int64_t>(g_http_servers.size())) return nullptr;
  return g_http_servers[id];
}
inline void _http_server_unregister(int64_t id) {
  if (id >= 0 && id < static_cast<int64_t>(g_http_servers.size()) &&
      g_http_servers[id]) {
    g_http_servers[id] = nullptr;
    g_http_server_free.push_back(id);
  }
}

// ---- Chunked response streaming (SSE / chunked) --------------------------
//
// A streaming handler returns a response Object carrying a `stream` Function.
// The backend wires set_chunked_content_provider; httplib then calls the
// provider (on this worker thread, where the runtime lives) to produce the
// body. The provider hands the script a `sink` handle whose .write(chunk)
// pushes one chunk. The sink is valid only during that one provider call, so
// scripts hold an id that encodes (slot, generation): a captured/escaped sink
// resolves stale and write() returns false instead of touching a dead DataSink.
// These helpers only touch DataSink (no httplib::Server/Client), so they pull
// no TLS symbol and stay outside the WEAK/STRONG gate.

// A slot+generation id registry over borrowed/owned `T*` handles: a captured or
// escaped id (used after the handle is invalidated, or after a slot is reused)
// resolves stale and fails safely with no ABA. The id encodes (gen<<32 | slot)
// in unsigned space (a signed shift into the sign bit would be UB). thread_local
// per registry instance — each handle is non-sendable. Shared by the streaming
// sink registry and the WebSocket connection registry.
template <class T>
struct IdRegistry {
  std::vector<T*> slots;
  std::vector<uint32_t> gen;
  std::vector<uint32_t> free;
  int64_t add(T* p) {
    uint32_t s;
    if (!free.empty()) {
      s = free.back();
      free.pop_back();
      slots[s] = p;
    } else {
      s = static_cast<uint32_t>(slots.size());
      slots.push_back(p);
      gen.push_back(0);
    }
    return static_cast<int64_t>((static_cast<uint64_t>(gen[s]) << 32) |
                                static_cast<uint64_t>(s));
  }
  T* get(int64_t id) {
    uint32_t s = static_cast<uint32_t>(id & 0xFFFFFFFF);
    uint32_t g = static_cast<uint32_t>(static_cast<uint64_t>(id) >> 32);
    if (s >= slots.size() || gen[s] != g) return nullptr;
    return slots[s];
  }
  void invalidate(int64_t id) {
    uint32_t s = static_cast<uint32_t>(id & 0xFFFFFFFF);
    if (s >= slots.size()) return;
    slots[s] = nullptr;
    gen[s]++;  // bump so a stale id (old generation) now fails
    free.push_back(s);
  }
};

inline thread_local IdRegistry<httplib::DataSink> g_http_sinks;

// Write a chunk through sink `id`. Returns false if the id is stale/invalid (an
// escaped sink) or the client has gone away — so the handler can stop early.
inline bool http_sink_write(int64_t id, const char* data, size_t len) {
  httplib::DataSink* s = g_http_sinks.get(id);
  if (!s) return false;
  return s->write(data, len);
}

// Backend-supplied stream runner: given a sink id, invoke the script's stream
// closure (which writes chunks via http_sink_write). Returns false if the
// closure threw — the connection is then aborted without a terminating chunk.
using StreamRunner = std::function<bool(int64_t sink_id)>;

// Wire `res` as a chunked stream of `content_type`, driven by `run`. httplib's
// chunked loop calls the provider repeatedly until done() is signaled; our
// provider drives the whole stream in one call, then sends the terminating
// chunk on success or aborts the connection on failure.
inline void http_server_set_stream(httplib::Response& res,
                                   const std::string& content_type,
                                   StreamRunner run) {
  res.set_chunked_content_provider(
      content_type,
      [run = std::move(run)](size_t /*offset*/,
                             httplib::DataSink& sink) -> bool {
        int64_t id = g_http_sinks.add(&sink);
        bool ok = run(id);
        g_http_sinks.invalidate(id);
        if (ok) {
          sink.done();  // terminating "0\r\n" chunk → ends the loop cleanly
          return true;
        }
        return false;  // closure threw → abort (no terminating chunk)
      });
}

#if !defined(CULEBRA_RT_HTTP_REQUEST_WEAK)

// A static asset mount: serve `dir` (a live disk dir in dev, or a baked
// embedded table under AOT) at the URL prefix `mount`.
struct StaticMount {
  std::string mount;
  std::unique_ptr<culebra::Dir> dir;
};

struct HttpServer {
  httplib::Server svr;
  // For listen_async: the background thread running the accept loop. Empty for
  // a blocking listen (which runs the accept loop on the caller's thread).
  std::thread accept_thread;
  // A server is single-use: once it has served (listen / listen_async), it
  // cannot be started again (httplib::Server is not designed to restart after
  // stop). Set on a successful start; a fresh start is rejected, not hung.
  bool started = false;
  // Static asset mounts (srv.static with Embed.dir), served by a pre_routing
  // handler installed on first use.
  std::vector<StaticMount> static_mounts;
  bool static_handler_installed = false;
};

// Default worker-pool size when the script doesn't pass `workers:`. Scaled with
// the machine but bounded: at least 4 (a browser opens several connections in
// parallel to load a page, and cpp-httplib holds each keep-alive connection on a
// worker for up to 5s — fewer workers serialize the page load) and at most 8
// (each worker carries its own culebra Runtime, so don't spawn one per core on a
// big box). hardware_concurrency() == 0 (unknown) → 4.
inline size_t default_http_workers() {
  unsigned hw = std::thread::hardware_concurrency();
  if (hw < 4) return 4;
  if (hw > 8) return 8;
  return hw;
}

// Backend hooks for a concurrent worker (run on each worker thread): `setup`
// builds the per-thread runtime + deserializes the route handlers into
// thread_local storage; `teardown` releases them before the runtime tears down.
// Value-neutral: the serialize/deserialize logic lives in the backend closures.
struct ServerWorkerHooks {
  std::function<void()> setup;
  std::function<void()> teardown;
};

// Concurrent task queue: a fixed pool of N worker threads, each owning its own
// culebra runtime (a heap/GC is thread_local and non-sendable). Each thread
// runs `hooks.setup()` once — under a RuntimeScope, so it deserializes the route
// handlers onto its own heap — then drains the job queue (httplib's
// process_and_close_socket, which routes to the C++ trampolines that read this
// thread's handler table). The accept loop polls on_idle for a cooperative
// Ctrl+C / cancel and stops the server, which joins the workers.
class CulebraWorkerPool : public httplib::TaskQueue {
 public:
  CulebraWorkerPool(size_t n, httplib::Server* svr,
                    std::atomic<bool>* isolate_flag, ServerWorkerHooks hooks)
      : svr_(svr), isolate_flag_(isolate_flag), hooks_(std::move(hooks)) {
    for (size_t i = 0; i < n; i++) threads_.emplace_back([this] { worker(); });
  }
  bool enqueue(std::function<void()> fn) override {
    {
      std::lock_guard<std::mutex> lk(m_);
      jobs_.push_back(std::move(fn));
    }
    cv_.notify_one();
    return true;
  }
  void on_idle() override {
    bool fired = culebra_g_sigint.load(std::memory_order_relaxed) ||
                 (isolate_flag_ && isolate_flag_ != &culebra_g_sigint &&
                  isolate_flag_->load(std::memory_order_relaxed));
    if (fired) svr_->stop();
  }
  void shutdown() override {
    {
      std::lock_guard<std::mutex> lk(m_);
      shutdown_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_)
      if (t.joinable()) t.join();
    threads_.clear();
  }

 private:
  void worker() {
    culebra::Runtime rt;
    culebra::RuntimeScope scope(rt);
    rt.interrupt_flag = isolate_flag_;  // honor cancel in nested blocking ops
    if (hooks_.setup) hooks_.setup();
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return shutdown_ || !jobs_.empty(); });
        if (shutdown_ && jobs_.empty()) break;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
    }
    if (hooks_.teardown) hooks_.teardown();
  }
  httplib::Server* svr_;
  std::atomic<bool>* isolate_flag_;
  ServerWorkerHooks hooks_;
  std::vector<std::thread> threads_;
  std::deque<std::function<void()>> jobs_;
  std::mutex m_;
  std::condition_variable cv_;
  bool shutdown_ = false;
};

// One WebSocket endpoint, unifying the two cpp-httplib types behind one shape so
// the backends call the same ws_* functions. A server endpoint borrows the
// httplib::ws::WebSocket the handler was handed (valid only during the handler); a client
// endpoint owns its httplib::ws::WebSocketClient (freed on drop). receive() collapses
// Text/Binary to a message string (binary-safe) and reports close as 0.
struct WsConn {
  httplib::ws::WebSocket* server = nullptr;                  // borrowed (server handler)
  std::unique_ptr<httplib::ws::WebSocketClient> client;      // owned (client handle)
  int receive(std::string& out) {
    httplib::ws::ReadResult r = server ? server->read(out) : client->read(out);
    return r == httplib::ws::Fail ? 0 : 1;  // Text/Binary → 1; Fail (closed) → 0
  }
  bool send(const char* d, size_t n) {
    std::string s(d, n);  // send as a Text frame (the std::string overload)
    return server ? server->send(s) : (client && client->send(s));
  }
  void close() {
    if (server) server->close();
    else if (client) client->close();
  }
  bool is_open() {
    return server ? server->is_open() : (client && client->is_open());
  }
};

// id → WsConn registry (same slot+generation scheme as the sink registry): a
// captured/escaped ws handle (used after its handler returned, or after close)
// resolves stale and its ops fail safely.
inline thread_local IdRegistry<WsConn> g_ws_conns;

#endif  // !CULEBRA_RT_HTTP_REQUEST_WEAK

// Create a server handle. Returns an id (>= 0); never fails in v0.
CULEBRA_RT_HTTP_LINKAGE int64_t http_server_open() {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  return -1;
#else
  return _http_server_register(new HttpServer());
#endif
}

// Register `handler` for `method` requests matching `pattern` (httplib route
// syntax, e.g. "/users/:id"). Unknown methods set `err`. A closed id no-ops.
CULEBRA_RT_HTTP_LINKAGE void http_server_route(int64_t id,
                                               const std::string& method,
                                               const std::string& pattern,
                                               RouteHandler handler,
                                               std::string& err) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)method; (void)pattern; (void)handler;
  err = "Http runtime not linked (no Http use detected at build)";
#else
  HttpServer* s = _http_server_get(id);
  if (!s) {
    err = "Http: server is closed";
    return;
  }
  if (method == "GET") s->svr.Get(pattern, std::move(handler));
  else if (method == "POST") s->svr.Post(pattern, std::move(handler));
  else if (method == "PUT") s->svr.Put(pattern, std::move(handler));
  else if (method == "DELETE") s->svr.Delete(pattern, std::move(handler));
  else if (method == "PATCH") s->svr.Patch(pattern, std::move(handler));
  else if (method == "OPTIONS") s->svr.Options(pattern, std::move(handler));
  else err = "Http: unsupported method: " + method;
#endif
}

// Serve files under `dir` at the URL prefix `mount`. `err` is set when the dir
// cannot be mounted. A closed id no-ops.
CULEBRA_RT_HTTP_LINKAGE void http_server_static(int64_t id,
                                                const std::string& mount,
                                                const std::string& dir,
                                                std::string& err) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)mount; (void)dir;
  err = "Http runtime not linked (no Http use detected at build)";
#else
  HttpServer* s = _http_server_get(id);
  if (!s) {
    err = "Http: server is closed";
    return;
  }
  if (!s->svr.set_mount_point(mount, dir)) {
    err = "Http: cannot serve directory: " + dir;
  }
#endif
}

// Serve a virtual directory at the URL prefix `mount`: the baked asset table
// registered under `name` when this binary was built with the assets embedded
// (AOT single binary), otherwise the live on-disk directory `base` (dev — edits
// show up on the next request). Static assets are tried before registered
// routes and only when the file exists, so an API route at e.g. /api/* still
// wins; a closed id no-ops. This is the `srv.static(mount, Embed.dir(...))`
// path — the plain `srv.static(mount, "dir")` keeps using set_mount_point.
CULEBRA_RT_HTTP_LINKAGE void http_server_serve_embed(int64_t id,
                                                     const std::string& mount,
                                                     const std::string& name,
                                                     std::string& err) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)mount; (void)name;
  err = "Http runtime not linked (no Http use detected at build)";
#else
  HttpServer* s = _http_server_get(id);
  if (!s) {
    err = "Http: server is closed";
    return;
  }
  std::unique_ptr<culebra::Dir> d;
  auto& tables = culebra::_asset_tables();
  if (auto it = tables.find(name); it != tables.end()) {
    d = std::make_unique<culebra::EmbeddedDir>(it->second.first,
                                               it->second.second);
  } else {
    // Dev: resolve the directory relative to the entry script (set at startup),
    // so it works regardless of the cwd — the same base the AOT build walks.
    std::string base = culebra::main_script_dir();
    if (!base.empty() && base.back() != '/') base += '/';
    base += name;
    d = std::make_unique<culebra::DiskDir>(base);
  }
  s->static_mounts.push_back({mount, std::move(d)});
  if (!s->static_handler_installed) {
    s->static_handler_installed = true;
    HttpServer* sp = s;  // outlives the handler (it lives on sp->svr)
    s->svr.set_pre_routing_handler(
        [sp](const httplib::Request& req,
             httplib::Response& res) -> httplib::Server::HandlerResponse {
          if (req.method != "GET" && req.method != "HEAD") {
            return httplib::Server::HandlerResponse::Unhandled;
          }
          for (auto& m : sp->static_mounts) {
            culebra::StaticResult r;
            if (culebra::serve_static(*m.dir, m.mount, req.path, r)) {
              res.status = r.status;
              // `r` is loop-local and unused after this — move the (possibly
              // large) body instead of letting set_content copy it.
              res.set_content(std::move(r.body), r.content_type);
              return httplib::Server::HandlerResponse::Handled;
            }
          }
          return httplib::Server::HandlerResponse::Unhandled;
        });
  }
#endif
}

// Bind to host:port and serve until stopped (Ctrl+C / isolate cancel). Blocks
// the calling thread. `n_workers <= 1` runs every handler inline on this thread
// (no serialization; the original closures); `n_workers > 1` runs a pool of that
// many worker threads, each with its own runtime built by `worker_setup`
// (deserialized route handlers) and torn down by `worker_teardown`. Returns
// false with `err` set when the bind fails. Honors a pending interrupt
// cooperatively on return (the same Interrupted the loop safepoint raises).
CULEBRA_RT_HTTP_LINKAGE bool http_server_listen(
    int64_t id, const std::string& host, int port, int n_workers,
    std::function<void()> worker_setup, std::function<void()> worker_teardown,
    std::string& err) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)host; (void)port; (void)n_workers;
  (void)worker_setup; (void)worker_teardown;
  err = "Http runtime not linked (no Http use detected at build)";
  return false;
#else
  HttpServer* s = _http_server_get(id);
  if (!s) {
    err = "Http: server is closed";
    return false;
  }
  if (s->started) {  // single-use: already served (this one or via listen_async)
    err = "Http: server already started (create a new server to serve again)";
    return false;
  }
  auto* isolate_flag = current_runtime().interrupt_flag;
  // Handlers always run on a worker pool (off the accept loop), so a slow
  // handler never blocks accepting new connections. n_workers < 1 picks the
  // CPU-scaled default. (No inline-on-accept-thread mode: that would stall all
  // accepts while one handler runs, and would let handlers share the caller's
  // mutable heap — both at odds with the isolate model.)
  size_t nw = n_workers >= 1 ? static_cast<size_t>(n_workers)
                             : default_http_workers();
  ServerWorkerHooks hooks{std::move(worker_setup), std::move(worker_teardown)};
  s->svr.new_task_queue = [nw, svr = &s->svr, isolate_flag,
                           hooks = std::move(hooks)] {
    return new CulebraWorkerPool(nw, svr, isolate_flag, hooks);
  };
  // Poll on_idle ~10x/s so a pending Ctrl+C stops the accept loop promptly even
  // when no connection arrives (idle_interval turns accept into a timed select).
  s->svr.set_idle_interval(0, 100 * 1000);
  // Bind first (so a bind failure leaves the handle reusable, like async), then
  // mark single-use and run the blocking accept loop on this thread.
  if (!s->svr.bind_to_port(host, port)) {
    err = "Http: failed to bind " + host + ":" + std::to_string(port);
    return false;
  }
  s->started = true;
  bool ok = s->svr.listen_after_bind();
  throw_if_interrupted();  // pending Ctrl+C / cancel → cooperative Interrupted
  if (!ok) err = "Http: serve loop failed";
  return ok;
#endif
}

// Bind and start serving on a background thread, returning immediately. Unlike
// the blocking listen, the accept loop (and handlers) run off the caller's
// thread, so handlers always run on a worker pool (>= 1 worker) and must be
// Sendable. The bind happens synchronously, so a bind failure is reported now
// (false + `err`); accept then runs on s->accept_thread until http_server_stop
// or close. on_idle still polls the interrupt flag so Ctrl+C / cancel stop it.
CULEBRA_RT_HTTP_LINKAGE bool http_server_listen_async(
    int64_t id, const std::string& host, int port, int n_workers,
    std::function<void()> worker_setup, std::function<void()> worker_teardown,
    std::string& err) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)host; (void)port; (void)n_workers;
  (void)worker_setup; (void)worker_teardown;
  err = "Http runtime not linked (no Http use detected at build)";
  return false;
#else
  HttpServer* s = _http_server_get(id);
  if (!s) {
    err = "Http: server is closed";
    return false;
  }
  // Single-use guard: a second start would move-assign onto a joinable
  // accept_thread (std::terminate) or restart a stopped httplib::Server (hangs).
  // Reject it as a catchable error — serve again with a fresh Http.server().
  if (s->started) {
    err = "Http: server already started (create a new server to serve again)";
    return false;
  }
  // The accept thread (and its workers) can outlive the caller's stack, so poll
  // the process-global sigint flag rather than a borrowed per-isolate flag whose
  // lifetime could be shorter. On the main thread these are the same pointer; an
  // isolate that uses listen_async stops it via the handle's drop/stop instead.
  auto* isolate_flag = &culebra_g_sigint;
  size_t nw = n_workers >= 1 ? static_cast<size_t>(n_workers)
                             : default_http_workers();
  ServerWorkerHooks hooks{std::move(worker_setup), std::move(worker_teardown)};
  s->svr.new_task_queue = [nw, svr = &s->svr, isolate_flag,
                           hooks = std::move(hooks)] {
    return new CulebraWorkerPool(nw, svr, isolate_flag, hooks);
  };
  s->svr.set_idle_interval(0, 100 * 1000);
  if (!s->svr.bind_to_port(host, port)) {
    err = "Http: failed to bind " + host + ":" + std::to_string(port);
    return false;  // bind failed before serving — handle stays reusable
  }
  s->started = true;
  s->accept_thread = std::thread([svr = &s->svr] { svr->listen_after_bind(); });
  return true;
#endif
}

// Stop a background (listen_async) server and join its accept thread. Idempotent;
// a closed/invalid id, or a blocking-listen server (no accept thread), no-ops.
// svr.stop() is cpp-httplib's cross-thread-safe shutdown, so this can be called
// from a different thread than the one serving.
CULEBRA_RT_HTTP_LINKAGE void http_server_stop(int64_t id) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id;
#else
  HttpServer* s = _http_server_get(id);
  if (!s) return;
  if (s->svr.is_running()) s->svr.stop();
  if (s->accept_thread.joinable()) s->accept_thread.join();
#endif
}

// Stop (if running) and free a server (idempotent; a closed/invalid id no-ops).
CULEBRA_RT_HTTP_LINKAGE void http_server_close(int64_t id) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id;
#else
  HttpServer* s = _http_server_get(id);
  if (!s) return;
  _http_server_unregister(id);
  if (s->svr.is_running()) s->svr.stop();
  if (s->accept_thread.joinable()) s->accept_thread.join();
  delete s;
#endif
}

// Register a WebSocket `handler` for connections matching `pattern`. The handler
// runs a long-lived loop for the connection (occupying its worker). A closed id
// no-ops.
CULEBRA_RT_HTTP_LINKAGE void http_server_ws(int64_t id,
                                            const std::string& pattern,
                                            WsHandler handler, std::string& err) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)pattern; (void)handler;
  err = "Http runtime not linked (no Http use detected at build)";
#else
  HttpServer* s = _http_server_get(id);
  if (!s) {
    err = "Http: server is closed";
    return;
  }
  s->svr.WebSocket(pattern, std::move(handler));
#endif
}

// Register a borrowed server-side WebSocket and return its id. The caller
// invalidates it (ws_unregister) when the handler returns.
CULEBRA_RT_HTTP_LINKAGE int64_t ws_register_server(void* ws) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)ws;
  return -1;
#else
  auto* conn = new WsConn();
  conn->server = static_cast<httplib::ws::WebSocket*>(ws);
  return g_ws_conns.add(conn);
#endif
}
CULEBRA_RT_HTTP_LINKAGE void ws_unregister(int64_t id) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id;
#else
  if (WsConn* c = g_ws_conns.get(id)) {
    g_ws_conns.invalidate(id);
    delete c;  // frees the WsConn (and, for a client, its WebSocketClient)
  }
#endif
}

// Connect a WebSocket client to `url` (ws://host:port/path). Returns an id, or
// -1 with `err` set on an invalid URL / failed connect.
CULEBRA_RT_HTTP_LINKAGE int64_t ws_client_open(const std::string& url,
                                               std::string& err) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)url;
  err = "Http runtime not linked (no Http use detected at build)";
  return -1;
#else
  auto client = std::make_unique<httplib::ws::WebSocketClient>(url);
  if (!client->is_valid()) {
    err = "Http.ws: invalid WebSocket URL: " + url;
    return -1;
  }
  if (!client->connect()) {
    err = "Http.ws: could not connect: " + url;
    return -1;
  }
  auto* conn = new WsConn();
  conn->client = std::move(client);
  return g_ws_conns.add(conn);
#endif
}

// Read the next message into `out`. Returns 1 on a message, 0 on close / a stale
// id (so a loop or for-in ends).
CULEBRA_RT_HTTP_LINKAGE int ws_receive(int64_t id, std::string& out) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)out;
  return 0;
#else
  WsConn* c = g_ws_conns.get(id);
  return c ? c->receive(out) : 0;
#endif
}

// Send a text message. Returns false on a stale id or a failed/closed socket.
CULEBRA_RT_HTTP_LINKAGE bool ws_send(int64_t id, const char* data, size_t len) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)data; (void)len;
  return false;
#else
  WsConn* c = g_ws_conns.get(id);
  return c ? c->send(data, len) : false;
#endif
}

// Close the connection (idempotent; a stale id no-ops).
CULEBRA_RT_HTTP_LINKAGE void ws_close(int64_t id) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id;
#else
  if (WsConn* c = g_ws_conns.get(id)) c->close();
#endif
}

CULEBRA_RT_HTTP_LINKAGE bool ws_is_open(int64_t id) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id;
  return false;
#else
  WsConn* c = g_ws_conns.get(id);
  return c ? c->is_open() : false;
#endif
}

}  // namespace culebra::http
