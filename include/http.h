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

#include <cctype>
#include <functional>
#include <httplib.h>
#include <string>
#include <utility>
#include <vector>

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

inline HttpResult http_request(const HttpRequest& req) {
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
  hreq.method = req.method;
  hreq.path = path;
  bool has_ct = false;
  for (const auto& [k, v] : req.headers) {
    hreq.headers.emplace(k, v);
    if (_iequals(k, "Content-Type")) has_ct = true;
  }
  if (req.body_source) {
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
      while (true) {
        if (!src(chunk)) {
          sink.done();
          return true;
        }
        if (!chunk.empty()) {
          sink.write(chunk.data(), chunk.size());
          return true;
        }
      }
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

  auto res = cli.send(hreq);
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

}  // namespace culebra::http
