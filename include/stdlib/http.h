#pragma once

// Type-neutral HTTP core for the Http namespace: both the client (a request)
// and server (a route handler) halves.
//
// No dependency on culebra Value / JitValue / GC: both the interp and JIT
// backends call http_request() and adapt HttpResult into their own object
// representation, and both build a server handler's request/response Object
// from the neutral ServerRequest/ServerResponse below. Keeping this header
// value-neutral lets the binding layer (stdlib_rt.h) include it without
// pulling each other in (mirrors proc.h) — and lets the core runtime archive
// (CULEBRA_RT_HTTP_REQUEST_WEAK) compile the whole binding surface without
// ever declaring an httplib type; see the include guard below.
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
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Included only where a body actually instantiates an httplib type (STRONG,
// or neither WEAK nor STRONG — the header-only / in-process JIT / http_test
// configurations). The core runtime archive (CULEBRA_RT_HTTP_REQUEST_WEAK)
// compiles the stubs alone: merely *declaring* httplib::SSLClient / SSLServer
// / SSLSocketStream emits their vtables (plus the digest-auth hashes) — 33
// undefined OpenSSL and 6 zlib references that ELF and Mach-O dead-strip but
// PE's ld reports before --gc-sections runs. Gating the include is what lets a
// Windows AOT binary stop linking OpenSSL/zlib unconditionally (src/main.cc,
// win_static) for a program that never uses Http.
#if !defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
#include <httplib.h>
#endif

#include <base/id_registry.h>    // IdRegistry<T> (slot+generation handle table)
#include <base/shared_tls.h>  // CULEBRA_RT_CORE_OWNED (one owner per thread_local)
#include <base/shared.h>  // throw_if_interrupted / culebra_g_sigint (Ctrl+C wiring)
#include <stdlib/vfs.h>     // Dir / DiskDir / EmbeddedDir / serve_static (static assets)

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
  int64_t timeout_sec = 0;            // per-phase timeout; 0 => library default.
  int64_t connect_timeout_sec = 0;    // connect only; 0 => timeout_sec.
  std::string proxy_host;          // empty => direct. Per request, never from
  int proxy_port = 0;              // $HTTPS_PROXY: a script's 127.0.0.1 call
                                   // must not go through a proxy.
  bool follow_redirects = true;    // 3xx Location chasing.
  BodySink body_sink = nullptr;    // set → stream the response body (no buffer).
  BodySource body_source = nullptr;// set → stream the request body (chunked).
  std::vector<MultipartPart> multipart;  // non-empty → multipart/form-data body.
};

struct HttpResult {
  bool ok = false;        // transport succeeded and a response was received.
  int64_t status = 0;     // HTTP status code (0 when ok is false).
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

// Percent-encode one URI component: RFC 3986's unreserved set plus `!*'()`,
// which is ECMA-262 encodeURIComponent — and what vendored cpp-httplib's
// encode_uri_component does. Those specs are the authority here, not httplib,
// so this rides culebra's own encoder rather than being gated on
// CULEBRA_RT_HTTP_REQUEST_WEAK: the Http bindings call it for `form:` bodies
// and the core archive compiles them too, so a weak stub would make a pure
// function silently depend on whether the Http feature archive got
// force-loaded. Byte-for-byte equivalence with httplib is asserted for all
// 256 byte values in test/http_test.cc.
inline std::string percent_encode_component(std::string_view s) {
  return culebra::percent_encode(s, "!*'()");
}

// Percent-encode name/value pairs as `k=v&k2=v2` (application/x-www-form-
// urlencoded form). Shared by query params (`?`+this) and `form:` bodies.
inline std::string encode_query(const HeaderList& pairs) {
  std::string q;
  for (const auto& [k, v] : pairs) {
    if (!q.empty()) q += "&";
    q += percent_encode_component(k) + "=" + percent_encode_component(v);
  }
  return q;
}

// http_request and the Http.client functions are the single OpenSSL/zlib choke:
// httplib::Client (the only code that pulls in TLS + gzip) is instantiated only
// inside the bodies below; on the server side, only the httplib adapters
// inside http_server_route/_ws and the http_res_* response setters
// setters touch httplib::Response. Their linkage is partitioned across the AOT
// runtime archives exactly like tensor_eval_node:
//   - core archive   (CULEBRA_RT_HTTP_REQUEST_WEAK):   weak stubs, never
//     touch httplib::Client/Server, so no CALL of ours reaches ssl/zlib —
//     and, since the binding layer (stdlib_rt.h) names only
//     the neutral ServerRequest/ServerResponse, this TU never *declares* an
//     httplib type either (httplib.h itself is `#if`-gated out above).
//   - http archive   (CULEBRA_RT_HTTP_REQUEST_STRONG): strong real bodies,
//     force-loaded only when the program uses Http (overrides the stubs).
//   - header-only / in-process JIT (neither): the normal inline bodies.
//
// A program that never uses Http therefore links no OpenSSL/zlib symbol at
// all (src/main.cc's win_static no longer appends CULEBRA_SSL_LINK
// unconditionally). This used to be impossible: including httplib.h emits
// SSLClient/SSLServer/SSLSocketStream vtables and the digest-auth hashes
// regardless of whether anything calls them (ELF/Mach-O dead-strip the
// resulting undefined OpenSSL references; PE's ld reports them before
// --gc-sections runs), and the binding layer used to name httplib::Request/
// Response directly, so the include could not be gated out. Moving the
// binding surface's *types* — not its TLS configuration — onto a neutral
// shape is what closes it: compiling this TU without TLS (e.g.
// -UCPPHTTPLIB_OPENSSL_SUPPORT for the core archive alone) is still not an
// option, since the two configurations would disagree about httplib's inline
// bodies and the linker folds them across the archives, segfaulting the
// first Http program built that way (measured).
//
// What --no-tls does instead is build a *second* strong object with TLS off
// (src/runtime/culebra_rt_http_notls.cc) and force-load that one in place of
// the first. The two never meet: the core archive holds neither, and a link
// takes exactly one. That is what makes the configuration split safe here and
// not one archive up.
#if defined(CULEBRA_RT_HTTP_REQUEST_STRONG)
#define CULEBRA_RT_HTTP_LINKAGE
#elif defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
#define CULEBRA_RT_HTTP_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_HTTP_LINKAGE inline
#endif

// IdRegistry<T> (id_registry.h): the slot+generation id table every handle
// table in this file (client, server, streaming sink, WebSocket connection)
// is built on, thread_local per instance since each handle is non-sendable.
// Included ahead of the client table (its first use) but outside the
// WEAK/STRONG gate that follows — a registry over T* never itself needs T to
// be complete, and the streaming sink table below needs it visible in the
// WEAK build too.

// Everything from here to the end of the client half is implementation: the
// handle table, the httplib-touching helpers and the HttpClient definition.
// The WEAK core archive's stubs never intern a handle, and a `thread_local`
// carried by both it and the force-loaded feature archive breaks the Windows
// AOT link (see rt_shared_tls.h), so none of it is compiled there.
#if !defined(CULEBRA_RT_HTTP_REQUEST_WEAK)

// A persistent client holding one reused keep-alive connection plus a base URL
// and default headers (the Http.client handle's native backing).
struct HttpClient;

// id → HttpClient* registry. Scripts hold a small integer id, never a raw
// pointer (matching the SQLite handle model — an invalid/forged id resolves to
// nullptr and fails safely instead of dereferencing arbitrary memory).
// thread_local because a client is non-sendable (one connection, one thread).
// A slot is reusable the moment a client closes, so without IdRegistry's
// generation check a captured/escaped id from a prior client could silently
// address a later, unrelated one on the same slot (no ABA).
inline thread_local IdRegistry<HttpClient> g_http_clients;
inline int64_t _http_client_register(HttpClient* c) { return g_http_clients.add(c); }
inline HttpClient* _http_client_get(int64_t id) { return g_http_clients.get(id); }
inline void _http_client_unregister(int64_t id) { g_http_clients.invalidate(id); }

// Append encoded query params to a path, preserving any query already in it.
inline void _http_append_params(std::string& path, const HeaderList& params) {
  if (params.empty()) return;
  path += path.find('?') == std::string::npos ? '?' : '&';
  path += encode_query(params);
}

// The one place request knobs become client settings. A one-off and a
// persistent client differ only in keep-alive: the one-off tolerates servers
// that don't speak keep-alive cleanly, the client reuses its connection.
inline void _http_configure(httplib::Client& cli, int64_t timeout_sec,
                            bool follow_redirects, bool keep_alive,
                            int64_t connect_timeout_sec = 0,
                            const std::string& proxy_host = {},
                            int proxy_port = 0) {
  cli.set_follow_location(follow_redirects);
  cli.set_keep_alive(keep_alive);
  if (timeout_sec > 0) {
    cli.set_read_timeout(timeout_sec, 0);
    cli.set_write_timeout(timeout_sec, 0);
  }
  auto connect = connect_timeout_sec > 0 ? connect_timeout_sec : timeout_sec;
  if (connect > 0) cli.set_connection_timeout(connect, 0);
  if (!proxy_host.empty()) cli.set_proxy(proxy_host.c_str(), proxy_port);
}

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
    // multipart/form-data. httplib's MultipartFormDataWriter gives us correct
    // boundary generation and RFC-7578 part framing; we drive it ourselves
    // (rather than Client::Post(items)) so the body still flows through the
    // single send(Request) choke and composes with params/headers/into.
    httplib::MultipartFormDataWriter writer;
    if (!has_ct) {
      hreq.headers.emplace("Content-Type", writer.content_type());
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
      hreq.body = writer.serialize(items);
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
        parts->push_back({writer.item_begin(meta), p.content, p.source});
      }
      hreq.headers.emplace("Transfer-Encoding", "chunked");
      hreq.is_chunked_content_provider_ = true;
      auto idx = std::make_shared<size_t>(0);
      auto in_body = std::make_shared<bool>(false);  // false → emit header next.
      hreq.content_provider_ =
          [parts, writer, idx, in_body](size_t, size_t,
                                        httplib::DataSink& sink) -> bool {
        const std::string crlf = httplib::MultipartFormDataWriter::item_end();
        if (*idx >= parts->size()) {
          std::string fin = writer.finish();
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
          sink.write(crlf.data(), crlf.size());
          ++*idx;
          *in_body = false;
          return true;
        }
        if (!sp.content.empty()) sink.write(sp.content.data(), sp.content.size());
        sink.write(crlf.data(), crlf.size());
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

// One watcher for the process, not one per request: the work is identical for
// every in-flight request, and a thread per request cost more than half of a
// loopback keep-alive request. It starts on the first request and parks on a
// condition variable when nothing is in flight.
class _InterruptWatcher {
 public:
  static _InterruptWatcher& instance() {
    static _InterruptWatcher w;
    return w;
  }

  // Registers `cli` for the duration of one send. `flag` is the calling
  // thread's isolate-cancel flag, captured by the caller: the watcher runs on
  // its own thread, where current_runtime() would be the wrong one.
  struct Guard {
    httplib::Client* cli;
    explicit Guard(httplib::Client& c, std::atomic<bool>* flag) : cli(&c) {
      instance().add(&c, flag);
    }
    ~Guard() { instance().remove(cli); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
  };

 private:
  struct Entry {
    httplib::Client* cli;
    std::atomic<bool>* flag;
  };

  _InterruptWatcher() = default;
  ~_InterruptWatcher() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
  }

  void add(httplib::Client* cli, std::atomic<bool>* flag) {
    std::unique_lock<std::mutex> lk(m_);
    live_.push_back({cli, flag});
    if (!th_.joinable()) th_ = std::thread([this] { run(); });
    // Woken only from parked: a wake costs the caller a futex wake and a
    // context switch, and a polling watcher already reacts within one tick.
    if (awake_ticks_ == 0) {
      awake_ticks_ = kLingerTicks;
      lk.unlock();
      cv_.notify_all();
    }
  }

  void remove(httplib::Client* cli) {
    std::lock_guard<std::mutex> lk(m_);
    std::erase_if(live_, [cli](const Entry& e) { return e.cli == cli; });
  }

  static constexpr auto kPoll = std::chrono::milliseconds(50);
  // Polls to keep going after the last request finishes. Requests come in
  // bursts, and staying awake across the gaps is what lets add() skip the wake;
  // a process that has stopped using Http goes quiet after 2 s.
  static constexpr int kLingerTicks = 40;

  void run() {
    std::unique_lock<std::mutex> lk(m_);
    while (!stop_) {
      if (awake_ticks_ == 0) { cv_.wait(lk); continue; }  // parked
      cv_.wait_for(lk, kPoll);
      // Keep calling stop() (not just once) so we still catch the socket if the
      // interrupt fires before it opens — e.g. mid DNS/connect. stop() is
      // cpp-httplib's documented thread-safe shutdown and does not call back in
      // here, so holding the registry lock across it cannot deadlock.
      for (const auto& e : live_) {
        if (interrupt_fired(e.flag)) e.cli->stop();
      }
      if (live_.empty()) --awake_ticks_; else awake_ticks_ = kLingerTicks;
    }
  }

  std::mutex m_;
  std::condition_variable cv_;
  std::vector<Entry> live_;
  std::thread th_;
  bool stop_ = false;
  // >0 while the watcher polls; 0 means parked, the only state add() has to
  // wake it out of. One field, so "parked" and "still lingering" cannot
  // disagree.
  int awake_ticks_ = 0;
};

// Send `hreq` over `cli` and map the result into an HttpResult, with cooperative
// Ctrl+C / isolate-cancel. send() blocks deep in the socket layer (connect,
// response-header wait, body) — not a runtime safepoint, and the internal
// poll/select swallow EINTR, so without the watcher a single SIGINT could not
// break a hung request. send() itself stays on THIS thread so the streaming
// callbacks (body_sink/body_source, which call back into culebra) run on the
// thread that owns the interpreter/JIT state. The watcher only reads the flag;
// throw_if_interrupted() afterwards raises the same cooperative Interrupted
// the loop safepoint does.
inline HttpResult _http_send(httplib::Client& cli, httplib::Request& hreq) {
  HttpResult out;
  _InterruptWatcher::Guard watch(cli, current_runtime().interrupt_flag);
  auto res = cli.send(hreq);
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
  int64_t timeout_sec = 0;         // carried so an absolute-URL one-off can reuse it.
  bool follow_redirects = true;
  std::mutex m;                 // one connection → serialize requests.
  explicit HttpClient(const std::string& origin) : cli(origin) {}
};

// True when this object cannot speak TLS and the URL asks it to. Without
// CPPHTTPLIB_OPENSSL_SUPPORT, httplib's Client constructor throws
// std::invalid_argument("'https' scheme is not supported.") — which reaches the
// user as a crash naming neither culebra nor the flag that made the binary this
// way. Refuse first instead, so `https://` is an ordinary HttpError.
//
// The TLS build compiles this too and always answers false: keeping one shape
// for both means the call sites below read the same in either configuration.
// It sits inside the non-weak region so it is confined to whichever feature
// object a binary force-loads — a helper shared with the core archive would be
// folded across the two configurations, the hazard the note above describes.
inline bool _http_tls_scheme_rejected(const std::string& url,
                                      std::string& err) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
  (void)url;
  (void)err;
  return false;
#else
  for (std::string_view scheme : {"https", "wss"}) {
    std::string prefix = std::string(scheme) + "://";
    if (url.size() >= prefix.size() &&
        _iequals(std::string_view(url).substr(0, prefix.size()), prefix)) {
      err = "Http: this binary was built with --no-tls, so " +
            std::string(scheme) + " URLs are not supported: " + url;
      return true;
    }
  }
  return false;
#endif
}

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
  if (_http_tls_scheme_rejected(req.url, err)) {
    out.error = std::move(err);
    return out;
  }
  _http_append_params(path, req.params);

  httplib::Client cli(origin);
  if (!cli.is_valid()) {
    out.error = "Http: invalid URL or unsupported scheme: " + req.url;
    return out;
  }
  _http_configure(cli, req.timeout_sec, req.follow_redirects,
                  /*keep_alive=*/false, req.connect_timeout_sec, req.proxy_host,
                  req.proxy_port);

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
                                                 int64_t timeout_sec,
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
  if (_http_tls_scheme_rejected(base_url, err)) return -1;
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
  _http_configure(c->cli, timeout_sec, follow_redirects, /*keep_alive=*/true);
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
  _http_append_params(path, req.params);
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
// reads the neutral ServerRequest and fills the ServerResponse. Handlers always
// run on a CulebraWorkerPool (one culebra runtime per worker thread; a
// value/GC is thread_local and non-sendable), never on the accept loop — so a
// slow handler can't block accepting new connections, and handlers must be
// Sendable.

// The non-owning counterpart of HeaderList: (key, value) views into whatever
// parsed them. Used for a received request, whose fields all point into the
// live httplib::Request.
using HeaderViews =
    std::vector<std::pair<std::string_view, std::string_view>>;

// One received request, mirrored out of httplib::Request by an adapter inside
// http_server_route/_ws (_http_make_request, below) so the binding layer — and
// therefore the core runtime archive — needs no httplib declaration.
//
// Every field is a view into the live httplib::Request, which outlives the
// handler call — nothing here is copied; the backends copy straight into their
// own values (the same single copy they always did). Only the three lists are
// materialized at all, because httplib's containers are httplib types, and the
// adapter builds them in iteration order, which is the order the backends
// insert into the Object they build.
struct ServerRequest {
  std::string_view method;  // "GET" / "POST" / ...
  std::string_view path;
  std::string_view body;
  HeaderViews headers;      // received headers
  HeaderViews params;       // parsed query string
  HeaderViews path_params;  // matched route parameters (":id" etc.)
};

// A borrowed handle on the httplib::Response a handler is filling — really an
// httplib::Response*, opaque here. http_res_set_status/header/content/stream
// (below) are the whole mutation surface and apply immediately (no buffering),
// so a handler that throws partway through a malformed response Object leaves
// exactly the fields it had already set — matching http_apply_response_meta's
// field-by-field application order.
//
// A wrapper around void* rather than an incomplete type (the shape
// WebSocketRef uses) because the adapter has to *create* one per request, and
// an incomplete type has no storage; going through void* keeps the cast a
// plain static_cast back to the original type.
struct ServerResponse {
  void* h;
};

// Opaque handle on a borrowed httplib::ws::WebSocket, only ever passed back to
// ws_register_server(void*) — never created here, so an incomplete type is
// enough and keeps the pointer type-checked. Declaring it pulls no TLS symbol,
// matching RouteHandler below.
struct WebSocketRef;

// Backend-supplied per-route handler: read the request, fill the response.
// Declaring it pulls no TLS symbol (only instantiating httplib::Server /
// httplib::Client does), so it stays ungated.
using RouteHandler =
    std::function<void(const ServerRequest&, ServerResponse&)>;

// Backend-supplied WebSocket handler: run a long-lived loop over the
// connection referenced by `ws` (via ws_send/ws_receive after
// ws_register_server(ws)) for one connection. httplib calls it on the worker
// thread, so it occupies that worker until it returns. Like RouteHandler,
// declaring it pulls no TLS symbol.
using WsHandler = std::function<void(const ServerRequest&, WebSocketRef*)>;

// id → HttpServer* registry, same opaque-pointer model as the client: scripts
// hold a small integer id, a forged/closed id resolves to nullptr and fails
// safely. thread_local because a server (v0) runs on the thread that created it.
// Gated with the bodies that intern a server, so the core archive does not also
// define the table (see rt_shared_tls.h). Same no-ABA reasoning as the client
// registry above.
#if !defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
struct HttpServer;
inline thread_local IdRegistry<HttpServer> g_http_servers;
inline int64_t _http_server_register(HttpServer* s) { return g_http_servers.add(s); }
inline HttpServer* _http_server_get(int64_t id) { return g_http_servers.get(id); }
inline void _http_server_unregister(int64_t id) { g_http_servers.invalidate(id); }
#endif  // !CULEBRA_RT_HTTP_REQUEST_WEAK

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

// Borrowed chunk writers, one per in-flight streaming response. What gets
// registered is the httplib::DataSink's own `write` member (which is exactly a
// BodySink) rather than the DataSink: pointing at the callback instead of its
// owner keeps this registry and http_sink_write free of any httplib type, so
// both stay ungated and the core archive keeps defining the registry (the
// feature archive borrows it — see rt_shared_tls.h).
CULEBRA_RT_CORE_OWNED thread_local IdRegistry<BodySink> g_http_sinks;

// Write a chunk through sink `id`. Returns false if the id is stale/invalid (an
// escaped sink) or the client has gone away — so the handler can stop early.
inline bool http_sink_write(int64_t id, const char* data, size_t len) {
  BodySink* w = g_http_sinks.get(id);
  if (!w) return false;
  return (*w)(data, len);
}

// Backend-supplied stream runner: given a sink id, invoke the script's stream
// closure (which writes chunks via http_sink_write). Returns false if the
// closure threw — the connection is then aborted without a terminating chunk.
using StreamRunner = std::function<bool(int64_t sink_id)>;

// http_res_set_status/header/content are the response half of the same choke
// as http_request (above): the only code that touches a real httplib::Response
// is here, linkage-split like every other server entry point. The WEAK stub in
// the core archive never runs — the route path that would call it
// (http_server_route/_ws) is itself a weak stub that never invokes a real
// handler — so it never actually dereferences res.h.
CULEBRA_RT_HTTP_LINKAGE void http_res_set_status(ServerResponse& res,
                                                 int status) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)res;
  (void)status;
#else
  static_cast<httplib::Response*>(res.h)->status = status;
#endif
}

CULEBRA_RT_HTTP_LINKAGE void http_res_set_header(ServerResponse& res,
                                                 const std::string& k,
                                                 const std::string& v) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)res;
  (void)k;
  (void)v;
#else
  static_cast<httplib::Response*>(res.h)->set_header(k, v);
#endif
}

// `body` is taken by value and moved into httplib's std::string&& overload of
// set_content — the same overload the call sites that build it (a temporary
// std::string) were already binding to, so this keeps the same copy count
// rather than adding one.
CULEBRA_RT_HTTP_LINKAGE void http_res_set_content(
    ServerResponse& res, std::string body, const std::string& content_type) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)res;
  (void)body;
  (void)content_type;
#else
  static_cast<httplib::Response*>(res.h)->set_content(std::move(body),
                                                       content_type);
#endif
}

// Wire `res` as a chunked stream of `content_type`, driven by `run`. httplib's
// chunked loop calls the provider repeatedly until done() is signaled; our
// provider drives the whole stream in one call, then sends the terminating
// chunk on success or aborts the connection on failure.
CULEBRA_RT_HTTP_LINKAGE void http_res_set_stream(ServerResponse& res,
                                                    const std::string& content_type,
                                                    StreamRunner run) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)res;
  (void)content_type;
  (void)run;
#else
  static_cast<httplib::Response*>(res.h)->set_chunked_content_provider(
      content_type,
      [run = std::move(run)](size_t /*offset*/,
                             httplib::DataSink& sink) -> bool {
        int64_t id = g_http_sinks.add(&sink.write);
        bool ok = run(id);
        g_http_sinks.invalidate(id);
        if (ok) {
          sink.done();  // terminating "0\r\n" chunk → ends the loop cleanly
          return true;
        }
        return false;  // closure threw → abort (no terminating chunk)
      });
#endif
}

#if !defined(CULEBRA_RT_HTTP_REQUEST_WEAK)

// A static asset mount: serve `dir` (a live disk dir in dev, or a baked
// embedded table under AOT) at the URL prefix `mount`.
struct StaticMount {
  std::string mount;
  std::unique_ptr<culebra::Dir> dir;
};

// Exclusive listen-socket options, replacing cpp-httplib's default. The default
// sets SO_REUSEPORT (Unix), which lets a second server bind a port with a live
// listener — the processes then silently shadow (macOS/BSD: first binder takes
// every connection) or split (Linux: kernel load-balances) each other's
// requests instead of the second bind failing. SO_REUSEADDR alone keeps
// restart-after-TIME_WAIT working but makes a live double-bind fail loudly.
// On Windows SO_REUSEADDR is the *hijack-enabling* option (different semantics
// than Unix); SO_EXCLUSIVEADDRUSE is the server-side equivalent of exclusivity.
inline void _http_exclusive_socket_options(socket_t sock) {
#ifdef _WIN32
  httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
  httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
}

struct HttpServer {
  httplib::Server svr;
  // For listen_async: the background thread running the accept loop. Empty for
  // a blocking listen (which runs the accept loop on the caller's thread).
  std::thread accept_thread;
  // A server is single-use: once it has served (listen / listen_async), it
  // cannot be started again (httplib::Server is not designed to restart after
  // stop). Set on a successful start; a fresh start is rejected, not hung.
  bool started = false;
  // The port the listening socket actually got, or -1 before bind. Held here
  // because bind and serve are separate calls: it is what tells serve the
  // socket is open, and (for a port-0 bind) the only record of the number.
  int bound_port = -1;
  // Static asset mounts (srv.static with Embed.dir), served by a pre_routing
  // handler installed on first use.
  std::vector<StaticMount> static_mounts;
  bool static_handler_installed = false;

  HttpServer() { svr.set_socket_options(_http_exclusive_socket_options); }
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
    if (interrupt_fired(isolate_flag_)) svr_->stop();
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
  std::vector<culebra::SizedThread> threads_;
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
    if (r == httplib::ws::Timeout) return -1;  // still open, nothing consumed
    return r == httplib::ws::Fail ? 0 : 1;     // Text/Binary → 1; Fail → 0
  }
  // 0 waits forever, as on a Socket. Only a connection that has one set can
  // report a timeout, which is what lets one thread both read and send here.
  void set_read_timeout(int64_t ms) {
    auto sec = static_cast<time_t>(ms / 1000);
    auto usec = static_cast<time_t>((ms % 1000) * 1000);
    if (server) server->set_read_timeout(sec, usec);
    else if (client) client->set_read_timeout(sec, usec);
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

// Mirror one httplib request into the neutral shape handed to the backends.
// Views throughout: `q` outlives the handler call. Iteration order from
// httplib's containers is preserved (see ServerRequest) — never sort or
// otherwise reorder here.
inline ServerRequest _http_make_request(const httplib::Request& q) {
  ServerRequest out;
  out.method = q.method;
  out.path = q.path;
  out.body = q.body;
  auto views = [](const auto& m) {
    HeaderViews v;
    v.reserve(m.size());
    for (const auto& [k, val] : m) v.emplace_back(k, val);
    return v;
  };
  out.headers = views(q.headers);
  out.params = views(q.params);
  out.path_params = views(q.path_params);
  return out;
}

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
  // Adapt once: fill the neutral request and hand the backend handler an
  // opaque response handle, whichever verb matches below.
  httplib::Server::Handler h =
      [handler = std::move(handler)](const httplib::Request& q,
                                     httplib::Response& r) {
        ServerResponse sr{&r};
        handler(_http_make_request(q), sr);
      };
  if (method == "GET") s->svr.Get(pattern, std::move(h));
  else if (method == "POST") s->svr.Post(pattern, std::move(h));
  else if (method == "PUT") s->svr.Put(pattern, std::move(h));
  else if (method == "DELETE") s->svr.Delete(pattern, std::move(h));
  else if (method == "PATCH") s->svr.Patch(pattern, std::move(h));
  else if (method == "OPTIONS") s->svr.Options(pattern, std::move(h));
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
  s->static_mounts.push_back({mount, culebra::open_embed_dir(name)});
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

inline constexpr const char* kHttpAlreadyStarted =
    "Http: server already started (create a new server to serve again)";

#if !defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
// Open and not yet served — what both bind and serve need before anything else.
// Single-use guard: a second start would move-assign onto a joinable
// accept_thread (std::terminate) or restart a stopped httplib::Server (hangs).
// Reject it as a catchable error — serve again with a fresh Http.server().
inline bool _http_server_open_unstarted(HttpServer* s, std::string& err) {
  if (!s) {
    err = "Http: server is closed";
    return false;
  }
  if (s->started) {
    err = kHttpAlreadyStarted;
    return false;
  }
  return true;
}
#endif

// Serving is two calls, `http_server_bind` then one of the two serve entries,
// because a blocking accept loop never returns and so can report nothing. Bind
// returns the port, which is what lets a caller log it or hand it to another
// isolate — and what makes a port-0 (OS-chosen) bind usable at all. Same split
// as Net.listen/serve, and as Go's net.Listen + http.Serve.
//
// Open the listening socket on host:port; `port` 0 asks the OS for an ephemeral
// one. Returns the port actually bound, or -1 with `err` set — on which the
// handle stays reusable, so a caller can try another port.
CULEBRA_RT_HTTP_LINKAGE int http_server_bind(int64_t id,
                                             const std::string& host, int port,
                                             std::string& err) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)host; (void)port;
  err = "Http runtime not linked (no Http use detected at build)";
  return -1;
#else
  HttpServer* s = _http_server_get(id);
  if (!_http_server_open_unstarted(s, err)) return -1;
  if (s->bound_port >= 0) {
    err = "Http: server already bound";
    return -1;
  }
  int bound = port == 0 ? s->svr.bind_to_any_port(host)
                        : (s->svr.bind_to_port(host, port) ? port : -1);
  if (bound < 0) {
    err = "Http: failed to bind " + host + ":" + std::to_string(port);
    // A failed bind decommissions the httplib server, and a decommissioned one
    // refuses every later bind. stop() is the only public way to clear that; on
    // a server that never ran it does nothing else. Without it "try the next
    // port" would fail on the second port for a reason the caller cannot see.
    s->svr.stop();
    return -1;
  }
  s->bound_port = bound;
  return bound;
#endif
}

// Whether a serve can proceed, checked ahead of the work. The binding layer
// calls this before it serializes handlers and installs route trampolines, so a
// serve that cannot happen leaves the handle exactly as it was — rather than
// half-registered with its recorded routes consumed.
CULEBRA_RT_HTTP_LINKAGE bool http_server_serve_ready(int64_t id,
                                                     std::string& err) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id;
  err = "Http runtime not linked (no Http use detected at build)";
  return false;
#else
  HttpServer* s = _http_server_get(id);
  if (!_http_server_open_unstarted(s, err)) return false;
  if (s->bound_port < 0) {
    err = "Http: server is not bound (call bind(port) first)";
    return false;
  }
  return true;
#endif
}

#if !defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
// Shared prologue for both serve paths: re-check the state, install the worker
// pool and the idle poll, and mark the handle single-use. Returns null with
// `err` set. `isolate_flag` is the only difference between the two (see the
// async note below). Handlers always run on the pool, off the accept loop, so a
// slow handler never blocks accepting new connections; n_workers < 1 picks the
// CPU-scaled default. (No inline-on-accept-thread mode: that would stall all
// accepts while one handler runs, and would let handlers share the caller's
// mutable heap — both at odds with the isolate model.)
inline HttpServer* _http_server_begin_serve(int64_t id, int n_workers,
                                            ServerWorkerHooks hooks,
                                            std::atomic<bool>* isolate_flag,
                                            std::string& err) {
  if (!http_server_serve_ready(id, err)) return nullptr;
  HttpServer* s = _http_server_get(id);
  size_t nw = n_workers >= 1 ? static_cast<size_t>(n_workers)
                             : default_http_workers();
  s->svr.new_task_queue = [nw, svr = &s->svr, isolate_flag,
                           hooks = std::move(hooks)] {
    return new CulebraWorkerPool(nw, svr, isolate_flag, hooks);
  };
  // Poll on_idle ~10x/s so a pending Ctrl+C stops the accept loop promptly even
  // when no connection arrives (idle_interval turns accept into a timed select).
  s->svr.set_idle_interval(0, 100 * 1000);
  s->started = true;
  return s;
}
#endif

// Run the accept loop on this thread until stopped (Ctrl+C / isolate cancel).
// Each worker thread gets its own runtime, built by `worker_setup` (deserialized
// route handlers) and torn down by `worker_teardown`. Honors a pending interrupt
// cooperatively on return (the same Interrupted the loop safepoint raises).
CULEBRA_RT_HTTP_LINKAGE bool http_server_serve(
    int64_t id, int n_workers, std::function<void()> worker_setup,
    std::function<void()> worker_teardown, std::string& err) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)n_workers; (void)worker_setup; (void)worker_teardown;
  err = "Http runtime not linked (no Http use detected at build)";
  return false;
#else
  HttpServer* s = _http_server_begin_serve(
      id, n_workers, {std::move(worker_setup), std::move(worker_teardown)},
      current_runtime().interrupt_flag, err);
  if (!s) return false;
  bool ok = s->svr.listen_after_bind();
  throw_if_interrupted();  // pending Ctrl+C / cancel → cooperative Interrupted
  if (!ok) err = "Http: serve loop failed";
  return ok;
#endif
}

// Run the accept loop on a background thread and return immediately. Handlers
// run off the caller's thread here too, so they must be Sendable either way;
// accept runs on s->accept_thread until http_server_stop or close.
CULEBRA_RT_HTTP_LINKAGE bool http_server_serve_async(
    int64_t id, int n_workers, std::function<void()> worker_setup,
    std::function<void()> worker_teardown, std::string& err) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)n_workers; (void)worker_setup; (void)worker_teardown;
  err = "Http runtime not linked (no Http use detected at build)";
  return false;
#else
  // The accept thread (and its workers) can outlive the caller's stack, so poll
  // the process-global sigint flag rather than a borrowed per-isolate flag whose
  // lifetime could be shorter. On the main thread these are the same pointer; an
  // isolate that serves in the background stops it via the handle's drop/stop.
  HttpServer* s = _http_server_begin_serve(
      id, n_workers, {std::move(worker_setup), std::move(worker_teardown)},
      &culebra_g_sigint, err);
  if (!s) return false;
  s->accept_thread = std::thread([svr = &s->svr] { svr->listen_after_bind(); });
  return true;
#endif
}

// Stop a background (serve_async) server and join its accept thread. Idempotent;
// a closed/invalid id, or a blocking-serve server (no accept thread), no-ops.
// svr.stop() is cpp-httplib's cross-thread-safe shutdown, so this can be called
// from a different thread than the one serving. Unconditional: it also releases
// the listening socket of a handle that bound and never served.
CULEBRA_RT_HTTP_LINKAGE void http_server_stop(int64_t id) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id;
#else
  HttpServer* s = _http_server_get(id);
  if (!s) return;
  s->svr.stop();
  if (s->accept_thread.joinable()) s->accept_thread.join();
#endif
}

// Stop and free a server (idempotent; a closed/invalid id no-ops).
CULEBRA_RT_HTTP_LINKAGE void http_server_close(int64_t id) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id;
#else
  HttpServer* s = _http_server_get(id);
  if (!s) return;
  _http_server_unregister(id);
  s->svr.stop();
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
  s->svr.WebSocket(
      pattern, [handler = std::move(handler)](const httplib::Request& q,
                                              httplib::ws::WebSocket& ws) {
        handler(_http_make_request(q), reinterpret_cast<WebSocketRef*>(&ws));
      });
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
  if (_http_tls_scheme_rejected(url, err)) return -1;
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
// id (so a loop or for-in ends), -1 when the read timeout elapsed with the
// connection still open.
CULEBRA_RT_HTTP_LINKAGE int ws_receive(int64_t id, std::string& out) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)out;
  return 0;
#else
  WsConn* c = g_ws_conns.get(id);
  return c ? c->receive(out) : 0;
#endif
}

// Bound how long a read waits before reporting a timeout (0 = forever).
// False on a stale id.
CULEBRA_RT_HTTP_LINKAGE bool ws_set_timeout(int64_t id, int64_t ms) {
#if defined(CULEBRA_RT_HTTP_REQUEST_WEAK)
  (void)id; (void)ms;
  return false;
#else
  WsConn* c = g_ws_conns.get(id);
  if (!c) return false;
  c->set_read_timeout(ms);
  return true;
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
