#pragma once

// Language Server Protocol server for Culebra — `culebra lsp`.
//
// An editor starts this and talks to it over stdio (Content-Length-framed
// JSON-RPC, the framing `culebra dap` uses). It publishes the diagnostics
// `culebra lint` reports, formats a document the way `culebra fmt` does, and
// answers hover over the names the reference docs carry. It reads source and
// never runs it.
//
// One thread only reads messages into a queue. The main thread does the rest —
// answers requests, and re-analyses a changed document once edits pause — so
// the parser (thread_local) and the process-wide test-ambient switch
// lint_source flips are only ever touched from one thread.

#include <cli/dap_json.h>
#include <cli/docs_cmd.h>
#include <cli/formatter.h>
#include <cli/framed_stdio.h>
#include <cli/lint_source.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace culebra::lsp {

using Json = dapjson::Value;  // the protocol's JSON: plain values, no interp

inline Json str(std::string s) { return Json(std::move(s)); }
inline Json num(int64_t n) { return Json(n); }
inline Json boolean(bool b) { return Json(b); }
inline Json object() {
  Json o;
  o.type = Json::Object;
  return o;
}
inline Json array() {
  Json a;
  a.type = Json::Array;
  return a;
}
inline const Json& at(const Json& v, std::string_view key) {
  static const Json kNil;
  const Json* f = v.type == Json::Object ? v.find(key) : nullptr;
  return f ? *f : kNil;
}

// ---- positions ------------------------------------------------------------
//
// culebra reports a 1-based line and a 1-based byte column. A client counts a
// 0-based line and a column in UTF-16 code units (LSP's default encoding, and
// the only one every client supports).

class LineIndex {
 public:
  explicit LineIndex(std::string_view text) : text_(text) {
    starts_.push_back(0);
    for (size_t i = 0; i < text.size(); i++)
      if (text[i] == '\n') starts_.push_back(i + 1);
  }
  size_t line_count() const { return starts_.size(); }
  // Line `l` (0-based) without its line ending; a CRLF's `\r` is the ending's.
  std::string_view line(size_t l) const {
    if (l >= starts_.size()) return {};
    size_t b = starts_[l];
    size_t e = l + 1 < starts_.size() ? starts_[l + 1] - 1 : text_.size();
    std::string_view s = text_.substr(b, e - b);
    if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
    return s;
  }

 private:
  std::string_view text_;
  std::vector<size_t> starts_;
};

inline size_t utf8_sequence_length(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;  // a stray continuation or invalid byte stands for itself
}

// UTF-16 code units in the first `bytes` bytes of `line`.
inline int64_t utf16_column(std::string_view line, size_t bytes) {
  int64_t units = 0;
  for (size_t i = 0; i < line.size() && i < bytes;) {
    size_t n = utf8_sequence_length(static_cast<unsigned char>(line[i]));
    if (i + n > line.size()) n = 1;
    units += n == 4 ? 2 : 1;
    i += n;
  }
  return units;
}

// The byte offset in `line` of a client's UTF-16 column, clamped to the line.
inline size_t byte_column(std::string_view line, int64_t units) {
  size_t i = 0;
  for (int64_t u = 0; i < line.size() && u < units;) {
    size_t n = utf8_sequence_length(static_cast<unsigned char>(line[i]));
    if (i + n > line.size()) n = 1;
    u += n == 4 ? 2 : 1;
    i += n;
  }
  return i;
}

inline Json position(size_t line, int64_t character) {
  Json p = object();
  p.set("line", num(static_cast<int64_t>(line)));
  p.set("character", num(character));
  return p;
}
inline Json make_range(size_t l0, int64_t c0, size_t l1, int64_t c1) {
  Json r = object();
  r.set("start", position(l0, c0));
  r.set("end", position(l1, c1));
  return r;
}

inline bool ident_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The range a diagnostic at culebra's (line, byte column) covers: the
// identifier starting there, or else the one character there.
inline Json diagnostic_range(const LineIndex& lines, int64_t line,
                             int64_t col) {
  size_t l = line > 0 ? static_cast<size_t>(line - 1) : 0;
  l = std::min(l, lines.line_count() - 1);
  std::string_view text = lines.line(l);
  size_t b = col > 0 ? std::min(static_cast<size_t>(col - 1), text.size()) : 0;
  size_t e = b;
  if (e < text.size() && ident_char(text[e])) {
    while (e < text.size() && ident_char(text[e])) e++;
  } else if (e < text.size()) {
    e += std::min(utf8_sequence_length(static_cast<unsigned char>(text[e])),
                  text.size() - e);
  }
  return make_range(l, utf16_column(text, b), l, utf16_column(text, e));
}

// A `file://` URI's path, percent-decoded. Anything else is passed through: the
// path only names the document in messages and decides whether it is a test.
inline std::string uri_to_path(std::string_view uri) {
  if (!uri.starts_with("file://")) return std::string(uri);
  std::string_view p = uri.substr(7);
  std::string out;
  for (size_t i = 0; i < p.size(); i++) {
    if (p[i] == '%' && i + 2 < p.size() &&
        std::isxdigit(static_cast<unsigned char>(p[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(p[i + 2]))) {
      out += static_cast<char>(
          std::stoi(std::string(p.substr(i + 1, 2)), nullptr, 16));
      i += 2;
    } else {
      out += p[i];
    }
  }
#if defined(_WIN32)
  // file:///C:/x names C:/x.
  if (out.size() >= 3 && out[0] == '/' &&
      std::isalpha(static_cast<unsigned char>(out[1])) && out[2] == ':')
    out.erase(0, 1);
#endif
  return out;
}

// The name a hover asks about: the identifier under `byte`, with the
// `Namespace.` qualifiers written before it (`Math.abs`). A member of a value
// (`xs.size`, `f().size`) is nothing here: which method it names depends on the
// value's type.
struct HoverName {
  std::string name;
  size_t begin;
  size_t end;
};
inline std::optional<HoverName> hover_name(std::string_view line, size_t byte) {
  size_t b = std::min(byte, line.size());
  if ((b == line.size() || !ident_char(line[b])) && b > 0 &&
      ident_char(line[b - 1]))
    b--;
  if (b >= line.size() || !ident_char(line[b])) return std::nullopt;
  size_t s = b, e = b;
  while (s > 0 && ident_char(line[s - 1])) s--;
  while (e < line.size() && ident_char(line[e])) e++;
  size_t word = s;
  while (s >= 2 && line[s - 1] == '.' && ident_char(line[s - 2])) {
    size_t q = s - 1;
    while (q > 0 && ident_char(line[q - 1])) q--;
    s = q;
  }
  if (s == word && s > 0 && line[s - 1] == '.') return std::nullopt;
  if (s != word && !std::isupper(static_cast<unsigned char>(line[s])))
    return std::nullopt;
  if (std::isdigit(static_cast<unsigned char>(line[s]))) return std::nullopt;
  return HoverName{std::string(line.substr(s, e - s)), s, e};
}

// ---- server ---------------------------------------------------------------

class Server {
 public:
  Server(int in_fd, int out_fd, std::string version)
      : channel_(std::make_shared<Channel>(in_fd, out_fd)),
        version_(std::move(version)) {}

  // Serve until the client sends `exit` or closes the stream. The exit code is
  // 0 after an orderly `shutdown` and 1 otherwise, as the protocol asks.
  int run() {
    // The reader holds the channel and nothing of the server's, so returning
    // from here while it is parked in a read is safe: the process ends it.
    std::thread([ch = channel_] {
      std::string body;
      while (ch->io.read(body)) ch->push(std::move(body));
      ch->push(std::nullopt);
    }).detach();

    for (;;) {
      auto next = channel_->pop(next_due());
      if (next.closed) return shutdown_ ? 0 : 1;
      if (next.message) {
        handle(*next.message);
        if (exit_) return shutdown_ ? 0 : 1;
      }
      analyze_due();
    }
  }

 private:
  using Clock = std::chrono::steady_clock;
  // How long edits must pause before a changed document is analysed again.
  static constexpr std::chrono::milliseconds kSettle{250};

  struct Channel {
    FramedStdio io;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::optional<std::string>> queue;  // nullopt: the stream ended

    Channel(int in_fd, int out_fd) : io(in_fd, out_fd) {}
    void push(std::optional<std::string> m) {
      {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(m));
      }
      cv.notify_one();
    }
    struct Next {
      std::optional<std::string> message;
      bool closed = false;
    };
    // The next message, waiting no later than `deadline` when there is one.
    Next pop(std::optional<Clock::time_point> deadline) {
      std::unique_lock<std::mutex> lk(mu);
      auto ready = [&] { return !queue.empty(); };
      if (deadline) {
        if (!cv.wait_until(lk, *deadline, ready)) return {};
      } else {
        cv.wait(lk, ready);
      }
      auto m = std::move(queue.front());
      queue.pop_front();
      if (!m) return {std::nullopt, true};
      return {std::move(m), false};
    }
  };

  struct Document {
    std::string text;
    Json version;                           // echoed with its diagnostics
    std::optional<Clock::time_point> due;   // a re-analysis waiting on edits
  };

  // ---- messages -----------------------------------------------------------
  void send(const Json& msg) { channel_->io.write(dapjson::stringify(msg)); }

  void reply(const Json& id, Json result) {
    Json r = object();
    r.set("jsonrpc", str("2.0"));
    r.set("id", id);
    r.set("result", std::move(result));
    send(r);
  }
  void reply_error(const Json& id, int64_t code, std::string message) {
    Json e = object();
    e.set("code", num(code));
    e.set("message", str(std::move(message)));
    Json r = object();
    r.set("jsonrpc", str("2.0"));
    r.set("id", id);
    r.set("error", std::move(e));
    send(r);
  }
  void notify(std::string method, Json params) {
    Json n = object();
    n.set("jsonrpc", str("2.0"));
    n.set("method", str(std::move(method)));
    n.set("params", std::move(params));
    send(n);
  }
  void log(std::string message) {
    Json p = object();
    p.set("type", num(1));  // Error
    p.set("message", str(std::move(message)));
    notify("window/logMessage", std::move(p));
  }

  void handle(const std::string& body) {
    Json msg;
    try {
      msg = dapjson::parse(body);
    } catch (const std::exception&) {
      reply_error(Json(), -32700, "parse error");
      return;
    }
    const Json& method = at(msg, "method");
    if (method.type != Json::String) return;  // a response: we send no requests
    Json id = at(msg, "id");
    bool request = id.type != Json::Nil;
    try {
      dispatch(method.to_string(), id, at(msg, "params"), request);
    } catch (const std::exception& e) {
      // A document that trips an internal error must not end the session.
      if (request)
        reply_error(id, -32603, e.what());
      else
        log(std::string("culebra lsp: ") + e.what());
    }
  }

  void dispatch(const std::string& m, const Json& id, const Json& params,
                bool request) {
    if (m == "initialize") return reply(id, initialize_result());
    if (m == "initialized") return;
    if (m == "shutdown") {
      shutdown_ = true;
      return reply(id, Json());
    }
    if (m == "exit") {
      exit_ = true;
      return;
    }
    if (m == "textDocument/didOpen") return did_open(params);
    if (m == "textDocument/didChange") return did_change(params);
    if (m == "textDocument/didSave") return did_save(params);
    if (m == "textDocument/didClose") return did_close(params);
    if (m == "textDocument/formatting") return reply(id, formatting(params));
    if (m == "textDocument/hover") return reply(id, hover(params));
    if (request) reply_error(id, -32601, "method not found: " + m);
  }

  Json initialize_result() {
    Json save = object();
    save.set("includeText", boolean(false));
    Json sync = object();
    sync.set("openClose", boolean(true));
    sync.set("change", num(1));  // Full: each change carries the whole text
    sync.set("save", std::move(save));
    Json caps = object();
    caps.set("textDocumentSync", std::move(sync));
    caps.set("documentFormattingProvider", boolean(true));
    caps.set("hoverProvider", boolean(true));
    Json info = object();
    info.set("name", str("culebra"));
    info.set("version", str(version_));
    Json r = object();
    r.set("capabilities", std::move(caps));
    r.set("serverInfo", std::move(info));
    return r;
  }

  // ---- documents ----------------------------------------------------------
  static const std::string& uri_of(const Json& params) {
    return at(at(params, "textDocument"), "uri").to_string();
  }

  void did_open(const Json& params) {
    const Json& td = at(params, "textDocument");
    if (at(td, "uri").type != Json::String) return;
    const std::string& uri = at(td, "uri").to_string();
    Document& d = docs_[uri];
    d.text = at(td, "text").to_string();
    d.version = at(td, "version");
    analyze(uri, d);
  }

  void did_change(const Json& params) {
    auto it = docs_.find(uri_of(params));
    if (it == docs_.end()) return;
    const Json& changes = at(params, "contentChanges");
    if (changes.type != Json::Array || changes.elements().empty()) return;
    it->second.text = at(changes.elements().back(), "text").to_string();
    it->second.version = at(at(params, "textDocument"), "version");
    it->second.due = Clock::now() + kSettle;
  }

  void did_save(const Json& params) {
    auto it = docs_.find(uri_of(params));
    if (it == docs_.end()) return;
    if (at(params, "text").type == Json::String)
      it->second.text = at(params, "text").to_string();
    analyze(it->first, it->second);
  }

  void did_close(const Json& params) {
    auto it = docs_.find(uri_of(params));
    if (it == docs_.end()) return;
    std::string uri = it->first;
    docs_.erase(it);
    Json p = object();
    p.set("uri", str(uri));
    p.set("diagnostics", array());
    notify("textDocument/publishDiagnostics", std::move(p));
  }

  std::optional<Clock::time_point> next_due() const {
    std::optional<Clock::time_point> due;
    for (const auto& [uri, d] : docs_)
      if (d.due && (!due || *d.due < *due)) due = d.due;
    return due;
  }

  void analyze_due() {
    auto now = Clock::now();
    for (auto& [uri, d] : docs_)
      if (d.due && *d.due <= now) analyze(uri, d);
  }

  void analyze(const std::string& uri, Document& d) {
    d.due.reset();
    std::string src = d.text;  // the parse normalizes it; positions use d.text
    auto linted = lint_source(uri_to_path(uri), src);
    LineIndex lines(d.text);
    Json diags = array();
    auto add = [&](int64_t line, int64_t col, const std::string& message,
                   int64_t severity, const std::string& code) {
      Json g = object();
      g.set("range", diagnostic_range(lines, line, col));
      g.set("severity", num(severity));
      g.set("source", str("culebra"));
      g.set("code", str(code));
      g.set("message", str(message));
      diags.elems.push_back(std::move(g));
    };
    if (linted.parsed) {
      for (const auto& x : linted.diagnostics)
        add(x.line, x.col, x.message,
            x.severity == lint::Severity::Error ? 1 : 2, x.kind);
    } else if (!linted.syntax_errors.empty()) {
      for (const auto& f : linted.syntax_errors)
        add(static_cast<int64_t>(f.line), static_cast<int64_t>(f.col),
            f.message, 1, "SyntaxError");
    } else {
      for (std::string m : linted.messages) {
        while (!m.empty() && m.back() == '\n') m.pop_back();
        add(1, 1, m, 1, "SyntaxError");
      }
    }
    Json p = object();
    p.set("uri", str(uri));
    if (d.version.type != Json::Nil) p.set("version", d.version);
    p.set("diagnostics", std::move(diags));
    notify("textDocument/publishDiagnostics", std::move(p));
  }

  // ---- requests -----------------------------------------------------------
  Json formatting(const Json& params) {
    auto it = docs_.find(uri_of(params));
    if (it == docs_.end()) return Json();
    auto r = fmt::format_source(uri_to_path(it->first), it->second.text);
    if (r.status == fmt::FormatStatus::Refused) log(r.message);
    // A parse error is already a diagnostic; unchanged text needs no edit.
    if (r.status != fmt::FormatStatus::Ok) return array();
    LineIndex lines(it->second.text);
    size_t last = lines.line_count() - 1;
    std::string_view tail = lines.line(last);
    Json edit = object();
    edit.set("range",
             make_range(0, 0, last, utf16_column(tail, tail.size())));
    edit.set("newText", str(std::move(r.output)));
    Json edits = array();
    edits.elems.push_back(std::move(edit));
    return edits;
  }

  Json hover(const Json& params) {
    auto it = docs_.find(uri_of(params));
    if (it == docs_.end()) return Json();
    const Json& pos = at(params, "position");
    int64_t line = at(pos, "line").to_long();
    LineIndex lines(it->second.text);
    if (line < 0 || static_cast<size_t>(line) >= lines.line_count())
      return Json();
    std::string_view text = lines.line(static_cast<size_t>(line));
    auto name =
        hover_name(text, byte_column(text, at(pos, "character").to_long()));
    if (!name) return Json();
    auto entry = find_doc_entry(name->name);
    if (!entry) return Json();
    std::string value = "```culebra\n" + entry->signature + "\n```";
    if (!entry->body.empty()) value += "\n\n" + entry->body;
    Json contents = object();
    contents.set("kind", str("markdown"));
    contents.set("value", str(std::move(value)));
    Json r = object();
    r.set("contents", std::move(contents));
    r.set("range", make_range(static_cast<size_t>(line),
                              utf16_column(text, name->begin),
                              static_cast<size_t>(line),
                              utf16_column(text, name->end)));
    return r;
  }

  std::shared_ptr<Channel> channel_;
  std::string version_;
  std::map<std::string, Document> docs_;  // by URI
  bool shutdown_ = false;
  bool exit_ = false;
};

}  // namespace culebra::lsp
