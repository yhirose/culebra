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
#include <cli/infer.h>
#include <cli/lint_source.h>
#include <frontend/module_loader.h>  // resolve_module_path
#include <frontend/resolve.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
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
  size_t size() const { return text_.size(); }
  size_t line_start(size_t l) const {
    return l < starts_.size() ? starts_[l] : text_.size();
  }
  // The (0-based) line a byte offset falls on.
  size_t line_of(size_t offset) const {
    auto it = std::upper_bound(starts_.begin(), starts_.end(), offset);
    return static_cast<size_t>(it - starts_.begin()) - 1;
  }

 private:
  std::string_view text_;
  std::vector<size_t> starts_;
};

// Byte length of the scalar at `s[i]`. An ill-formed byte stands for itself,
// as an editor decoding the same bytes shows it as one U+FFFD.
inline size_t utf8_sequence_length(std::string_view s, size_t i) {
  size_t n = unicode::utf8::codepoint_length(s.data() + i, s.size() - i);
  return n ? n : 1;
}

// UTF-16 code units of a scalar `n` UTF-8 bytes long. Only a 4-byte scalar
// lies outside the BMP and takes a surrogate pair.
inline int64_t utf16_units(size_t n) { return n == 4 ? 2 : 1; }

// UTF-16 code units in the first `bytes` bytes of `line`.
inline int64_t utf16_column(std::string_view line, size_t bytes) {
  int64_t units = 0;
  for (size_t i = 0; i < line.size() && i < bytes;) {
    size_t n = utf8_sequence_length(line, i);
    units += utf16_units(n);
    i += n;
  }
  return units;
}

// The byte offset in `line` of a client's UTF-16 column, clamped to the line.
inline size_t byte_column(std::string_view line, int64_t units) {
  size_t i = 0;
  for (int64_t u = 0; i < line.size() && u < units;) {
    size_t n = utf8_sequence_length(line, i);
    u += utf16_units(n);
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
    e += utf8_sequence_length(text, e);
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

// The `file://` URI of a path, percent-encoding what a URI cannot carry.
inline std::string path_to_uri(std::string path) {
#if defined(_WIN32)
  for (char& c : path)
    if (c == '\\') c = '/';
  if (!path.empty() && path[0] != '/') path.insert(path.begin(), '/');
#endif
  return "file://" + percent_encode(path, "/:");
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
  // `catalog` supplies the stdlib to type inference, built on first use;
  // without one, completion knows only what the document declares.
  Server(int in_fd, int out_fd, std::string version,
         std::function<const infer::Catalog*()> catalog = {})
      : channel_(std::make_shared<Channel>(in_fd, out_fd)),
        version_(std::move(version)),
        catalog_provider_(std::move(catalog)) {}

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

  // The last text of a document that parsed, and what its names resolved to.
  // Navigation reads it while the buffer is mid-edit and does not parse; a
  // rename insists that it still matches the buffer.
  struct Snapshot {
    std::string text;  // as the parse normalized it; the AST views it
    std::shared_ptr<peg::Ast> ast;
    resolve::Resolution res;
    std::vector<resolve::OutlineItem> outline;

    const LineIndex& lines() const {
      if (!line_index) line_index = std::make_unique<const LineIndex>(text);
      return *line_index;
    }

   private:
    mutable std::unique_ptr<const LineIndex> line_index;
  };

  struct Document {
    std::string text;
    Json version;                           // echoed with its diagnostics
    std::optional<Clock::time_point> due;   // a re-analysis waiting on edits
    std::shared_ptr<const Snapshot> snapshot;
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
    if (m == "textDocument/completion") return reply(id, completion(params));
    if (m == "textDocument/definition") return reply(id, definition(params));
    if (m == "textDocument/references") return reply(id, references(params));
    if (m == "textDocument/documentHighlight")
      return reply(id, highlights(params));
    if (m == "textDocument/documentSymbol") return reply(id, outline(params));
    if (m == "textDocument/prepareRename") return prepare_rename(id, params);
    if (m == "textDocument/rename") return rename(id, params);
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
    Json triggers = array();
    triggers.elems.push_back(str("."));
    Json completion_caps = object();
    completion_caps.set("triggerCharacters", std::move(triggers));
    caps.set("completionProvider", std::move(completion_caps));
    caps.set("definitionProvider", boolean(true));
    caps.set("referencesProvider", boolean(true));
    caps.set("documentHighlightProvider", boolean(true));
    caps.set("documentSymbolProvider", boolean(true));
    Json rename_caps = object();
    rename_caps.set("prepareProvider", boolean(true));
    caps.set("renameProvider", std::move(rename_caps));
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
    // The parse normalizes the text it reads and the AST views it, so it reads
    // the snapshot's own copy; diagnostics are placed in the buffer's text.
    auto snap = std::make_shared<Snapshot>();
    snap->text = d.text;
    auto linted = lint_source(uri_to_path(uri), snap->text);
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
    if (linted.authored) {
      snap->ast = linted.authored;
      snap->res = resolve::resolve_module(*snap->ast, snap->text);
      snap->outline = resolve::outline(*snap->ast, snap->text, snap->res);
      d.snapshot = std::move(snap);
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
    if (Json typed = symbol_hover(params); typed.type != Json::Nil) return typed;
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
    return markdown_hover(
        std::move(value),
        make_range(static_cast<size_t>(line), utf16_column(text, name->begin),
                   static_cast<size_t>(line), utf16_column(text, name->end)));
  }

  // ---- navigation ---------------------------------------------------------
  // The document a request names, analysed first if an edit is still settling.
  Document* current(const Json& params) {
    auto it = docs_.find(uri_of(params));
    if (it == docs_.end()) return nullptr;
    if (it->second.due) analyze(it->first, it->second);
    return &it->second;
  }

  static size_t offset_at(const LineIndex& lines, const Json& pos) {
    int64_t line = at(pos, "line").to_long();
    if (line < 0) return 0;
    size_t l = static_cast<size_t>(line);
    if (l >= lines.line_count()) return lines.size();
    return lines.line_start(l) +
           byte_column(lines.line(l), at(pos, "character").to_long());
  }

  static Json span_range(const LineIndex& lines, size_t begin, size_t end) {
    size_t l0 = lines.line_of(begin), l1 = lines.line_of(end);
    return make_range(
        l0, utf16_column(lines.line(l0), begin - lines.line_start(l0)), l1,
        utf16_column(lines.line(l1), end - lines.line_start(l1)));
  }

  static Json location(const std::string& uri, const LineIndex& lines,
                       size_t begin, size_t end) {
    Json loc = object();
    loc.set("uri", str(uri));
    loc.set("range", span_range(lines, begin, end));
    return loc;
  }

  // A symbol's occurrences, one per position (a keyword label can name the
  // same parameter of several overloads).
  template <class F>
  static void for_each_occurrence(const resolve::Resolution& res, size_t symbol,
                                  F&& f) {
    size_t last = resolve::kNone;
    for (size_t i : res.symbols[symbol].occurrences) {
      const auto& x = res.occurrences[i];
      if (x.position == last) continue;
      last = x.position;
      f(x);
    }
  }

  static Json markdown_hover(std::string value, Json range) {
    Json contents = object();
    contents.set("kind", str("markdown"));
    contents.set("value", str(std::move(value)));
    Json r = object();
    r.set("contents", std::move(contents));
    r.set("range", std::move(range));
    return r;
  }

  // The name under the request's position in the document's last snapshot.
  struct Target {
    Document* doc = nullptr;
    const Snapshot* snap = nullptr;
    const resolve::Occurrence* occ = nullptr;
    size_t offset = 0;
  };
  Target target(const Json& params) {
    Target t;
    t.doc = current(params);
    if (!t.doc || !t.doc->snapshot) return t;
    t.snap = t.doc->snapshot.get();
    const LineIndex& lines = t.snap->lines();
    t.offset = offset_at(lines, at(params, "position"));
    t.occ = t.snap->res.occurrence_at(t.offset);
    return t;
  }

  Json definition(const Json& params) {
    Target t = target(params);
    if (!t.snap) return Json();
    const std::string& uri = uri_of(params);
    const resolve::Resolution& res = t.snap->res;
    if (const auto* m = res.module_member_at(t.offset))
      return module_definition(uri, *t.snap, m->import_symbol, m->member);
    if (!t.occ) return Json();
    const resolve::Symbol& sym = res.symbols[t.occ->symbol];
    if (sym.kind == resolve::SymbolKind::Import &&
        t.occ->role == resolve::Role::Declaration)
      return module_definition(uri, *t.snap, t.occ->symbol, "");
    const LineIndex& lines = t.snap->lines();
    Json locs = array();
    for (size_t i : sym.occurrences) {
      const auto& x = res.occurrences[i];
      if (x.role == resolve::Role::Declaration)
        locs.elems.push_back(
            location(uri, lines, x.position, x.position + x.length));
    }
    return locs;
  }

  // Where an import's module declares `member` at its top level, or the
  // module's first line when `member` is empty or not declared there.
  Json module_definition(const std::string& uri, const Snapshot& snap,
                         size_t import_symbol, const std::string& member) {
    const resolve::Import* imp = nullptr;
    for (const auto& i : snap.res.imports)
      if (i.symbol == import_symbol) imp = &i;
    Module* mod = imp ? module_for(uri, imp->path) : nullptr;
    if (!mod) return Json();
    std::string target_uri = path_to_uri(mod->path);
    LineIndex lines(mod->text);
    Json locs = array();
    if (auto it = mod->res.scopes[0].names.find(member);
        !member.empty() && it != mod->res.scopes[0].names.end())
      for (size_t i : mod->res.symbols[it->second].occurrences) {
        const auto& x = mod->res.occurrences[i];
        if (x.role == resolve::Role::Declaration)
          locs.elems.push_back(
              location(target_uri, lines, x.position, x.position + x.length));
      }
    if (locs.elems.empty()) {
      Json loc = object();
      loc.set("uri", str(target_uri));
      loc.set("range", make_range(0, 0, 0, 0));
      locs.elems.push_back(std::move(loc));
    }
    return locs;
  }

  Json references(const Json& params) {
    Target t = target(params);
    if (!t.occ) return Json();
    const std::string& uri = uri_of(params);
    const resolve::Resolution& res = t.snap->res;
    bool with_declaration =
        at(at(params, "context"), "includeDeclaration").to_bool();
    const LineIndex& lines = t.snap->lines();
    Json locs = array();
    for_each_occurrence(res, t.occ->symbol, [&](const resolve::Occurrence& x) {
      if (!with_declaration && x.role == resolve::Role::Declaration) return;
      locs.elems.push_back(location(uri, lines, x.position, x.position + x.length));
    });
    return locs;
  }

  Json highlights(const Json& params) {
    Target t = target(params);
    if (!t.occ) return Json();
    const resolve::Resolution& res = t.snap->res;
    const LineIndex& lines = t.snap->lines();
    Json out = array();
    for_each_occurrence(res, t.occ->symbol, [&](const resolve::Occurrence& x) {
      Json h = object();
      h.set("range", span_range(lines, x.position, x.position + x.length));
      h.set("kind", num(x.role == resolve::Role::Read ? 2 : 3));  // Read : Write
      out.elems.push_back(std::move(h));
    });
    return out;
  }

  static int64_t symbol_kind(resolve::OutlineKind k) {
    switch (k) {  // LSP SymbolKind
      case resolve::OutlineKind::Function: return 12;
      case resolve::OutlineKind::Class: return 5;
      case resolve::OutlineKind::Constructor: return 9;
      case resolve::OutlineKind::Method: return 6;
      case resolve::OutlineKind::Field: return 8;
      case resolve::OutlineKind::Enum: return 10;
      case resolve::OutlineKind::EnumMember: return 22;
      case resolve::OutlineKind::Trait: return 11;
      case resolve::OutlineKind::Variable: return 13;
      case resolve::OutlineKind::Module: return 2;
      case resolve::OutlineKind::EffectOperation: return 24;
    }
    return 13;
  }

  static Json outline_item(const LineIndex& lines,
                           const resolve::OutlineItem& it) {
    Json o = object();
    o.set("name", str(it.name));
    o.set("kind", num(symbol_kind(it.kind)));
    o.set("range", span_range(lines, it.begin, it.end));
    o.set("selectionRange", span_range(lines, it.name_begin, it.name_end));
    if (!it.children.empty()) {
      Json children = array();
      for (const auto& c : it.children)
        children.elems.push_back(outline_item(lines, c));
      o.set("children", std::move(children));
    }
    return o;
  }

  Json outline(const Json& params) {
    Document* d = current(params);
    if (!d || !d->snapshot) return Json();
    const LineIndex& lines = d->snapshot->lines();
    Json out = array();
    for (const auto& it : d->snapshot->outline)
      out.elems.push_back(outline_item(lines, it));
    return out;
  }

  // ---- rename -------------------------------------------------------------
  // Why the name a request points at cannot be renamed, or "" when it can.
  static std::string rename_blocker(const Target& t) {
    std::string buffer = t.doc->text;
    try {
      normalize_source_newlines(buffer);
    } catch (const std::exception&) {
      buffer.clear();
    }
    if (buffer != t.snap->text)
      return "The document does not parse as it stands. Rename works on the "
             "last version that parsed, so fix the syntax error first.";
    if (!t.occ) return "There is no name here to rename.";
    const resolve::Symbol& sym = t.snap->res.symbols[t.occ->symbol];
    if (sym.exported)
      return std::format("`{}` is exported, and the modules that import it "
                         "would not follow the rename.",
                         sym.name);
    if (t.snap->res.called_as_method(sym.name))
      return std::format("`{}` is also called as a method (`value.{}(...)`). "
                         "Those calls may reach this name, and a rename "
                         "cannot follow them.",
                         sym.name, sym.name);
    return "";
  }

  static bool valid_name(std::string_view n) {
    if (n.empty() || std::isdigit(static_cast<unsigned char>(n[0])))
      return false;
    return std::all_of(n.begin(), n.end(), ident_char);
  }

  // Why `name` cannot replace the symbol's name, or "" when it can.
  static std::string name_conflict(const Snapshot& snap, size_t symbol,
                                   const std::string& name) {
    const resolve::Resolution& res = snap.res;
    const resolve::Symbol& sym = res.symbols[symbol];
    if (!valid_name(name) || name == "_")
      return std::format("`{}` is not a name.", name);
    if (is_keyword(name)) return std::format("`{}` is a keyword.", name);
    if (is_always_bound_name(name))
      return std::format("`{}` is a name the language binds itself.", name);
    if (name == sym.name) return "";
    const LineIndex& lines = snap.lines();
    for (size_t i : sym.occurrences) {
      const auto& x = res.occurrences[i];
      size_t other = res.lookup(x.scope, name);
      if (other != resolve::kNone && other != symbol)
        return std::format("`{}` already names something visible on line {}.",
                           name, lines.line_of(x.position) + 1);
    }
    // A global of that name read inside the symbol's scope would start reading
    // the renamed variable instead.
    for (const auto& u : res.unresolved) {
      if (u.name != name) continue;
      for (size_t sc = u.scope; sc != resolve::kNone; sc = res.scopes[sc].parent)
        if (sc == sym.scope)
          return std::format("`{}` is read on line {}, where the renamed name "
                             "would take its place.",
                             name, lines.line_of(u.position) + 1);
    }
    return "";
  }

  void prepare_rename(const Json& id, const Json& params) {
    Target t = target(params);
    if (!t.snap) return reply(id, Json());
    std::string why = rename_blocker(t);
    if (!why.empty()) return reply_error(id, -32803, why);  // RequestFailed
    const LineIndex& lines = t.snap->lines();
    Json r = object();
    r.set("range",
          span_range(lines, t.occ->position, t.occ->position + t.occ->length));
    r.set("placeholder", str(t.snap->res.symbols[t.occ->symbol].name));
    reply(id, std::move(r));
  }

  void rename(const Json& id, const Json& params) {
    Target t = target(params);
    if (!t.snap) return reply(id, Json());
    std::string why = rename_blocker(t);
    if (why.empty())
      why = name_conflict(*t.snap, t.occ->symbol,
                          at(params, "newName").to_string());
    if (!why.empty()) return reply_error(id, -32803, why);
    const std::string& name = at(params, "newName").to_string();
    const resolve::Resolution& res = t.snap->res;
    const resolve::Symbol& sym = res.symbols[t.occ->symbol];
    const LineIndex& lines = t.snap->lines();
    Json edits = array();
    for_each_occurrence(res, t.occ->symbol, [&](const resolve::Occurrence& x) {
      // `{x}` keeps its key: it becomes `{x: renamed}`.
      bool shorthand = x.spelling == resolve::Spelling::ObjectShorthand ||
                       x.spelling == resolve::Spelling::PatternShorthand;
      Json e = object();
      e.set("range", span_range(lines, x.position, x.position + x.length));
      e.set("newText", str(shorthand ? sym.name + ": " + name : name));
      edits.elems.push_back(std::move(e));
    });
    Json changes = object();
    changes.set(uri_of(params), std::move(edits));
    Json r = object();
    r.set("changes", std::move(changes));
    reply(id, std::move(r));
  }

  // ---- completion ---------------------------------------------------------
  // The name completion puts where the half-typed one was, so the buffer parses.
  static constexpr std::string_view kPlaceholder = "__culebra_complete__";

  static int64_t completion_kind(infer::MemberKind k) {
    switch (k) {  // LSP CompletionItemKind
      case infer::MemberKind::Method: return 2;
      case infer::MemberKind::Function: return 3;
      case infer::MemberKind::Constructor: return 4;
      case infer::MemberKind::Field: return 5;
      case infer::MemberKind::Variable:
      case infer::MemberKind::Parameter: return 6;
      case infer::MemberKind::Class: return 7;
      case infer::MemberKind::Namespace:
      case infer::MemberKind::Module: return 9;
      case infer::MemberKind::Enum: return 13;
      case infer::MemberKind::EnumMember: return 20;
      case infer::MemberKind::Constant: return 21;
    }
    return 6;
  }

  const infer::Catalog* catalog() const {
    if (!catalog_ && catalog_provider_) catalog_ = catalog_provider_();
    return catalog_;
  }

  // An imported module, parsed and inferred once per text it has. The types
  // its members carry point into this entry, which lives until that text
  // changes, so they stay valid across the requests that read them.
  struct Module {
    std::string path;
    std::string source;  // as read, to tell a change
    std::string text;    // the parse's copy, which it normalizes and the AST views
    std::shared_ptr<peg::Ast> ast;
    resolve::Resolution res;
    std::unique_ptr<infer::Inference> inference;
    std::vector<infer::Member> members;
  };

  // An import, from its open buffer or its file, parsed and inferred once per
  // text it has; nullptr when it cannot be read or parsed.
  Module* module_for(const std::string& uri, std::string_view import_path) {
    auto path = resolve_module_path(
        std::string(import_path),
        std::filesystem::path(uri_to_path(uri)).parent_path());
    std::string source;
    if (auto it = docs_.find(path_to_uri(path.string())); it != docs_.end()) {
      source = it->second.text;
    } else {
      std::ifstream in(path, std::ios::binary);
      if (!in) return nullptr;
      std::stringstream ss;
      ss << in.rdbuf();
      source = ss.str();
    }
    const std::string key = path.string();
    if (auto it = modules_.find(key);
        it != modules_.end() && it->second->source == source)
      return it->second.get();
    auto mod = std::make_unique<Module>();
    mod->path = key;
    mod->source = source;
    mod->text = std::move(source);
    std::vector<ParseFailure> failures;
    mod->ast = parse(key, mod->text, failures);
    if (!mod->ast) {
      modules_.erase(key);
      return nullptr;
    }
    mod->res = resolve::resolve_module(*mod->ast, mod->text);
    mod->inference = std::make_unique<infer::Inference>(*mod->ast, mod->text,
                                                        mod->res, catalog());
    mod->members = mod->inference->visible(mod->text.size());
    auto& slot = modules_[key];
    slot = std::move(mod);
    return slot.get();
  }

  infer::Inference::ModuleMembers module_members_for(const std::string& uri) {
    return [this, uri](std::string_view import_path) {
      Module* mod = module_for(uri, import_path);
      return mod ? mod->members : std::vector<infer::Member>{};
    };
  }

  // Members leave the inference that listed them: settle a deferred field
  // type into its detail, and drop what pointed into that inference.
  static void settle(std::vector<infer::Member>& ms, const infer::Inference& inf) {
    for (auto& m : ms) {
      if (m.kind == infer::MemberKind::Field && m.detail == m.name) {
        infer::Type t = inf.member_type(m);
        if (!t.unknown()) m.detail = m.name + ": " + t.to_string();
      }
      if (m.owner == &inf) {
        m.owner = nullptr;
        m.function = m.value = m.field_class = nullptr;
      }
    }
  }

  static const peg::Ast* placeholder_call(const peg::Ast& n, size_t& index) {
    using namespace peg::udl;
    if (n.tag == "CALL"_)
      for (size_t i = 1; i < n.nodes.size(); i++)
        if (n.nodes[i]->tag == "IDENTIFIER"_ && n.nodes[i]->token == kPlaceholder) {
          index = i;
          return &n;
        }
    for (const auto& c : n.nodes)
      if (const auto* found = placeholder_call(*c, index)) return found;
    return nullptr;
  }

  Json completion(const Json& params) {
    // Not `current()`: the placeholder parse reads the buffer as it is, so an
    // edit still settling need not be analysed first.
    auto doc = docs_.find(uri_of(params));
    if (doc == docs_.end()) return Json();
    const Document& d = doc->second;
    const std::string& uri = doc->first;
    LineIndex buffer(d.text);
    size_t cursor = offset_at(buffer, at(params, "position"));
    size_t start = cursor;
    while (start > 0 && ident_char(d.text[start - 1])) start--;
    bool member = start > 0 && d.text[start - 1] == '.';

    // Complete against the buffer with the half-typed name replaced by a
    // placeholder, which parses where the buffer mid-edit usually does not;
    // failing that, against the last version that parsed, where the cursor's
    // offset is a close guess.
    std::string probe = d.text.substr(0, start) + std::string(kPlaceholder) +
                        d.text.substr(cursor);
    std::vector<ParseFailure> failures;
    std::shared_ptr<peg::Ast> ast = parse(uri_to_path(uri), probe, failures);
    std::optional<resolve::Resolution> probe_res;
    std::optional<infer::Inference> inf;
    size_t offset = 0;
    std::optional<infer::Type> receiver;
    if (ast) {
      probe_res.emplace(resolve::resolve_module(*ast, probe));
      inf.emplace(*ast, probe, *probe_res, catalog(), module_members_for(uri));
      offset = probe.find(kPlaceholder);
      size_t index = 0;
      if (member)
        if (const peg::Ast* call = placeholder_call(*ast, index))
          receiver = inf->chain_type(*call, index);
    } else if (const Snapshot* s = d.snapshot.get(); s && s->ast) {
      inf.emplace(*s->ast, s->text, s->res, catalog(), module_members_for(uri));
      offset = std::min(start, s->text.size());
      if (member) {
        size_t end = start - 1, begin = end;
        while (begin > 0 && ident_char(d.text[begin - 1])) begin--;
        std::string name = d.text.substr(begin, end - begin);
        size_t sym = s->res.lookup(s->res.scope_at(offset), name);
        if (sym != resolve::kNone) {
          receiver = inf->symbol_type(sym);
        } else if (catalog() && catalog()->namespace_members(name)) {
          infer::Alt a{infer::Kind::Namespace};
          a.name = name;
          receiver = infer::Type::single(std::move(a));
        }
      }
    }

    std::vector<infer::Member> primary, secondary;
    if (inf) {
      if (member) {
        if (receiver) primary = inf->members(*receiver);
        secondary = inf->ufcs(offset);
      } else {
        primary = inf->visible(offset);
        if (catalog()) secondary = catalog()->globals();
      }
      settle(primary, *inf);
      settle(secondary, *inf);
    }

    Json items = array();
    std::set<std::string, std::less<>> seen;
    auto emit = [&](const std::vector<infer::Member>& ms, char rank,
                    bool functions_only) {
      for (const auto& m : ms) {
        if (functions_only && m.kind != infer::MemberKind::Function) continue;
        if (!seen.insert(m.name).second) continue;
        Json item = object();
        item.set("label", str(m.name));
        item.set("kind", num(completion_kind(m.kind)));
        if (!m.detail.empty()) item.set("detail", str(m.detail));
        item.set("sortText", str(std::string(1, rank) + m.name));
        items.elems.push_back(std::move(item));
      }
    };
    // A member of the value's type first, then a free function UFCS reaches.
    emit(primary, '0', false);
    emit(secondary, '1', member);
    Json r = object();
    r.set("isIncomplete", boolean(false));
    r.set("items", std::move(items));
    return r;
  }

  // A hover over a name the document declares: its declaration and type.
  Json symbol_hover(const Json& params) {
    Target t = target(params);
    if (!t.occ || !t.snap->ast) return Json();
    infer::Inference inf(*t.snap->ast, t.snap->text, t.snap->res, catalog(),
                         module_members_for(uri_of(params)));
    const LineIndex& lines = t.snap->lines();
    return markdown_hover(
        "```culebra\n" + inf.describe(t.occ->symbol) + "\n```",
        span_range(lines, t.occ->position, t.occ->position + t.occ->length));
  }

  std::shared_ptr<Channel> channel_;
  std::string version_;
  std::function<const infer::Catalog*()> catalog_provider_;
  mutable const infer::Catalog* catalog_ = nullptr;
  std::map<std::string, Document> docs_;  // by URI
  std::map<std::string, std::unique_ptr<Module>> modules_;  // imports, by path
  bool shutdown_ = false;
  bool exit_ = false;
};

}  // namespace culebra::lsp
