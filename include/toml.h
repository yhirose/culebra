#pragma once

// TOML parse / serialize for the `TOML` stdlib namespace.
//
// Value-neutral core (no culebra types), shared by the interp
// (stdlib_interp.h) and the JIT/AOT runtime helpers (stdlib_jit.h) so the
// three backends agree byte-for-byte. Parsing produces a neutral `Node`
// tree (Table / Array / String / Int / Float / Bool); each backend converts
// that tree to its own Value type. Serialization is the dual: a backend
// converts its value into a `Node` tree and this core renders the text.
//
// Scope: TOML v1.0 keys (bare + quoted + dotted), `[table]` /
// `[[array.of.tables]]` headers, the four string forms (basic, literal,
// multiline basic, multiline literal), integers (dec/hex/oct/bin with `_`),
// floats (incl. inf/nan), booleans, arrays, and inline tables. Date-times
// are kept as their raw String (CSV-style value-neutral: no Instant type),
// so `stringify(parse(t))` re-quotes them as ordinary strings. Binary safe.
//
// Malformed input throws `toml::ParseError{message, line, col}` (1-based);
// each backend rethrows it as a culebra `ValueError` with `e.line`/`e.col`.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace culebra::toml {

enum class Kind { Table, Array, String, Int, Float, Bool };

// A neutral TOML value. Tables keep insertion order (a vector of pairs) so
// round-trips and diffs are stable, matching JSON/CSV's ordered behaviour.
struct Node {
  Kind kind = Kind::Table;
  std::vector<std::pair<std::string, Node>> items;  // Table entries (ordered)
  std::vector<Node> elems;                          // Array elements
  std::string s;                                    // String (and raw datetime)
  int64_t i = 0;
  double f = 0.0;
  bool b = false;

  static Node table() { return Node{}; }
  static Node array() { Node n; n.kind = Kind::Array; return n; }
  static Node string(std::string v) { Node n; n.kind = Kind::String; n.s = std::move(v); return n; }
  static Node integer(int64_t v) { Node n; n.kind = Kind::Int; n.i = v; return n; }
  static Node floating(double v) { Node n; n.kind = Kind::Float; n.f = v; return n; }
  static Node boolean(bool v) { Node n; n.kind = Kind::Bool; n.b = v; return n; }

  Node* find(std::string_view key) {
    for (auto& kv : items)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  Node& set(std::string key, Node v) {
    items.emplace_back(std::move(key), std::move(v));
    return items.back().second;
  }
};

struct ParseError {
  std::string message;
  int64_t line;
  int64_t col;
};

// --- parser ---------------------------------------------------------------

class Parser {
 public:
  Parser(const char* begin, const char* end) : p_(begin), end_(end) {}

  static bool is_bare_key_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
  }

  // Parse a whole document into the root table.
  Node parse_document() {
    Node root = Node::table();
    Node* current = &root;  // where bare keys land
    for (;;) {
      skip_ws_and_comments_and_newlines();
      if (p_ >= end_) break;
      if (*p_ == '[') {
        current = parse_header(root);
      } else {
        parse_keyval(*current);
      }
      skip_inline_ws();
      skip_comment();
      if (p_ < end_) {
        if (*p_ == '\n' || *p_ == '\r') {
          consume_newline();
        } else {
          fail("expected newline after entry");
        }
      }
    }
    return root;
  }

 private:
  const char* p_;
  const char* end_;
  long line_ = 1;
  long col_ = 1;

  [[noreturn]] void fail(const std::string& msg) {
    throw ParseError{msg, line_, col_};
  }

  void advance() {
    if (*p_ == '\n') { line_++; col_ = 1; } else { col_++; }
    ++p_;
  }
  bool eof() const { return p_ >= end_; }
  char peek() const { return *p_; }
  char peek(size_t k) const { return (p_ + k < end_) ? p_[k] : '\0'; }

  void skip_inline_ws() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t')) advance();
  }
  void skip_comment() {
    if (p_ < end_ && *p_ == '#') {
      while (p_ < end_ && *p_ != '\n') advance();
    }
  }
  void consume_newline() {
    if (*p_ == '\r' && p_ + 1 < end_ && p_[1] == '\n') advance();
    advance();
  }
  void skip_ws_and_comments_and_newlines() {
    for (;;) {
      while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r'))
        advance();
      if (p_ < end_ && *p_ == '#') { skip_comment(); continue; }
      break;
    }
  }

  // Read one key segment: bare key or a quoted ("..." / '...') key.
  std::string parse_key_segment() {
    if (p_ >= end_) fail("expected key");
    if (*p_ == '"' || *p_ == '\'') return parse_string_value().s;
    std::string out;
    while (p_ < end_ && is_bare_key_char(*p_)) { out += *p_; advance(); }
    if (out.empty()) fail("expected key");
    return out;
  }

  // Read a dotted key path (`a.b.c`) into segments.
  std::vector<std::string> parse_key_path() {
    std::vector<std::string> path;
    for (;;) {
      skip_inline_ws();
      path.push_back(parse_key_segment());
      skip_inline_ws();
      if (p_ < end_ && *p_ == '.') { advance(); continue; }
      break;
    }
    return path;
  }

  // Descend/create the intermediate tables of a dotted path, returning the
  // table that should hold the final key. Errors if a segment collides with
  // a non-table value.
  Node* descend(Node& root, const std::vector<std::string>& path, size_t upto) {
    Node* cur = &root;
    for (size_t k = 0; k < upto; k++) {
      Node* nxt = cur->find(path[k]);
      if (!nxt) {
        nxt = &cur->set(path[k], Node::table());
      } else if (nxt->kind == Kind::Array && !nxt->elems.empty() &&
                 nxt->elems.back().kind == Kind::Table) {
        nxt = &nxt->elems.back();  // dotted into the latest array-of-tables entry
      } else if (nxt->kind != Kind::Table) {
        fail("key '" + path[k] + "' is not a table");
      }
      cur = nxt;
    }
    return cur;
  }

  // `[a.b]` selects/creates a table; `[[a.b]]` appends a table to an array.
  // Returns the table subsequent bare keys belong to.
  Node* parse_header(Node& root) {
    advance();  // consume '['
    bool array_of_tables = (p_ < end_ && *p_ == '[');
    if (array_of_tables) advance();
    auto path = parse_key_path();
    skip_inline_ws();
    if (p_ >= end_ || *p_ != ']') fail("expected ']'");
    advance();
    if (array_of_tables) {
      if (p_ >= end_ || *p_ != ']') fail("expected ']]'");
      advance();
    }
    if (path.empty()) fail("empty table header");
    Node* parent = descend(root, path, path.size() - 1);
    const std::string& leaf = path.back();
    if (array_of_tables) {
      Node* arr = parent->find(leaf);
      if (!arr) arr = &parent->set(leaf, Node::array());
      if (arr->kind != Kind::Array) fail("key '" + leaf + "' is not an array");
      arr->elems.push_back(Node::table());
      return &arr->elems.back();
    }
    Node* tbl = parent->find(leaf);
    if (!tbl) tbl = &parent->set(leaf, Node::table());
    if (tbl->kind != Kind::Table) fail("key '" + leaf + "' is not a table");
    return tbl;
  }

  // `key = value` (key may be dotted) into table `dest`.
  void parse_keyval(Node& dest) {
    auto path = parse_key_path();
    skip_inline_ws();
    if (p_ >= end_ || *p_ != '=') fail("expected '='");
    advance();
    skip_inline_ws();
    Node val = parse_value();
    Node* parent = descend(dest, path, path.size() - 1);
    if (parent->find(path.back())) fail("duplicate key '" + path.back() + "'");
    parent->set(path.back(), std::move(val));
  }

  Node parse_value() {
    if (p_ >= end_) fail("expected value");
    char c = *p_;
    if (c == '"' || c == '\'') return parse_string_value();
    if (c == '[') return parse_array();
    if (c == '{') return parse_inline_table();
    if (c >= '0' && c <= '9') {
      if (size_t len = match_datetime()) {
        std::string raw(p_, p_ + len);
        for (size_t k = 0; k < len; k++) advance();
        return Node::string(std::move(raw));
      }
    }
    return parse_atom();
  }

  // Append a Unicode code point as UTF-8 bytes.
  static void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
      out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  // Decode a `\uXXXX` / `\UXXXXXXXX` escape body of `digits` hex chars.
  void read_unicode_escape(std::string& out, int digits) {
    uint32_t cp = 0;
    for (int k = 0; k < digits; k++) {
      if (p_ >= end_) fail("bad unicode escape");
      char h = *p_;
      cp <<= 4;
      if (h >= '0' && h <= '9') cp |= (h - '0');
      else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
      else fail("bad unicode escape");
      advance();
    }
    append_utf8(out, cp);
  }

  // Parse any of the four string forms; returns a String node.
  Node parse_string_value() {
    char q = *p_;
    bool triple = (peek(1) == q && peek(2) == q);
    if (triple) {
      advance(); advance(); advance();
      return q == '"' ? parse_multiline_basic() : parse_multiline_literal();
    }
    return q == '"' ? parse_basic() : parse_literal();
  }

  Node parse_basic() {
    advance();  // opening "
    std::string out;
    while (p_ < end_ && *p_ != '"') {
      if (*p_ == '\n') fail("unterminated string");
      if (*p_ == '\\') { read_escape(out); continue; }
      out += *p_; advance();
    }
    if (p_ >= end_) fail("unterminated string");
    advance();  // closing "
    return Node::string(std::move(out));
  }

  Node parse_literal() {
    advance();  // opening '
    std::string out;
    while (p_ < end_ && *p_ != '\'') {
      if (*p_ == '\n') fail("unterminated string");
      out += *p_; advance();
    }
    if (p_ >= end_) fail("unterminated string");
    advance();  // closing '
    return Node::string(std::move(out));
  }

  Node parse_multiline_basic() {
    // A newline immediately after the opening delimiter is trimmed.
    if (p_ < end_ && (*p_ == '\n' || *p_ == '\r')) consume_newline();
    std::string out;
    while (p_ < end_) {
      if (*p_ == '"' && peek(1) == '"' && peek(2) == '"') {
        advance(); advance(); advance();
        return Node::string(std::move(out));
      }
      if (*p_ == '\\') {
        // A backslash followed only by whitespace then a newline is a line
        // continuation: trim the backslash, newline, and the next line's
        // leading whitespace.
        const char* q = p_ + 1;
        while (q < end_ && (*q == ' ' || *q == '\t')) q++;
        if (q < end_ && (*q == '\n' || *q == '\r')) {
          advance();  // consume backslash
          while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' ||
                               *p_ == '\r'))
            advance();
          continue;
        }
        read_escape(out);
        continue;
      }
      out += *p_; advance();
    }
    fail("unterminated multiline string");
  }

  Node parse_multiline_literal() {
    if (p_ < end_ && (*p_ == '\n' || *p_ == '\r')) consume_newline();
    std::string out;
    while (p_ < end_) {
      if (*p_ == '\'' && peek(1) == '\'' && peek(2) == '\'') {
        advance(); advance(); advance();
        return Node::string(std::move(out));
      }
      out += *p_; advance();
    }
    fail("unterminated multiline string");
  }

  // Decode one `\x` escape (already positioned at the backslash).
  void read_escape(std::string& out) {
    advance();  // consume '\'
    if (p_ >= end_) fail("bad escape");
    char e = *p_;
    switch (e) {
      case '"':  out += '"';  advance(); break;
      case '\\': out += '\\'; advance(); break;
      case 'b':  out += '\b'; advance(); break;
      case 't':  out += '\t'; advance(); break;
      case 'n':  out += '\n'; advance(); break;
      case 'f':  out += '\f'; advance(); break;
      case 'r':  out += '\r'; advance(); break;
      case 'u':  advance(); read_unicode_escape(out, 4); break;
      case 'U':  advance(); read_unicode_escape(out, 8); break;
      default:   fail("bad escape");
    }
  }

  Node parse_array() {
    advance();  // '['
    Node arr = Node::array();
    for (;;) {
      skip_ws_and_comments_and_newlines();
      if (p_ >= end_) fail("unterminated array");
      if (*p_ == ']') { advance(); return arr; }
      arr.elems.push_back(parse_value());
      skip_ws_and_comments_and_newlines();
      if (p_ < end_ && *p_ == ',') { advance(); continue; }
      skip_ws_and_comments_and_newlines();
      if (p_ < end_ && *p_ == ']') { advance(); return arr; }
      fail("expected ',' or ']'");
    }
  }

  Node parse_inline_table() {
    advance();  // '{'
    Node tbl = Node::table();
    skip_inline_ws();
    if (p_ < end_ && *p_ == '}') { advance(); return tbl; }
    for (;;) {
      parse_keyval(tbl);  // same key = value handling as a top-level entry
      skip_inline_ws();
      if (p_ < end_ && *p_ == ',') { advance(); continue; }
      skip_inline_ws();
      if (p_ < end_ && *p_ == '}') { advance(); return tbl; }
      fail("expected ',' or '}'");
    }
  }

  static bool is_value_terminator(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' ||
           c == ']' || c == '}' || c == '#';
  }

  // Parse a bare bool / integer / float token.
  Node parse_atom() {
    const char* start = p_;
    while (p_ < end_ && !is_value_terminator(*p_)) advance();
    std::string tok(start, p_);
    if (tok.empty()) fail("expected value");
    if (tok == "true") return Node::boolean(true);
    if (tok == "false") return Node::boolean(false);
    return classify_number(tok);
  }

  Node classify_number(const std::string& tok) {
    if (tok == "inf" || tok == "+inf") return Node::floating(INFINITY);
    if (tok == "-inf") return Node::floating(-INFINITY);
    if (tok == "nan" || tok == "+nan" || tok == "-nan") return Node::floating(NAN);

    // Radix-prefixed integers: 0x / 0o / 0b (no underscores stripped yet).
    if (tok.size() > 2 && tok[0] == '0' &&
        (tok[1] == 'x' || tok[1] == 'o' || tok[1] == 'b')) {
      int base = tok[1] == 'x' ? 16 : (tok[1] == 'o' ? 8 : 2);
      std::string digits = strip_underscores(tok.substr(2));
      int64_t v = 0;
      auto res = std::from_chars(digits.data(), digits.data() + digits.size(),
                                 v, base);
      if (res.ec != std::errc{} || res.ptr != digits.data() + digits.size())
        fail("invalid integer");
      return Node::integer(v);
    }

    std::string clean = strip_underscores(tok);
    bool is_float = clean.find('.') != std::string::npos ||
                    clean.find('e') != std::string::npos ||
                    clean.find('E') != std::string::npos;
    if (is_float) {
      // std::from_chars for double is unavailable on older libc++ (macOS),
      // so use strtod over the null-terminated cleaned token.
      const char* cs = clean.c_str();
      char* endp = nullptr;
      double d = std::strtod(cs, &endp);
      if (endp != cs + clean.size()) fail("invalid float");
      return Node::floating(d);
    }
    int64_t v = 0;
    const char* b = clean.data();
    if (!clean.empty() && clean[0] == '+') b++;  // from_chars rejects leading +
    auto res = std::from_chars(b, clean.data() + clean.size(), v);
    if (res.ec != std::errc{} || res.ptr != clean.data() + clean.size())
      fail("invalid value");
    return Node::integer(v);
  }

  static std::string strip_underscores(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
      if (c != '_') out += c;
    return out;
  }

  static bool digit(char c) { return c >= '0' && c <= '9'; }

  // If the input at p_ begins a TOML date / time / date-time, return its byte
  // length (so it can be captured raw as a String); otherwise 0. A space
  // separator is only consumed when a time actually follows the date.
  size_t match_datetime() {
    const char* q = p_;
    auto have = [&](size_t k) { return q + k < end_; };
    auto d = [&](size_t k) { return have(k) && digit(q[k]); };

    // Full date: dddd-dd-dd
    if (d(0) && d(1) && d(2) && d(3) && have(4) && q[4] == '-' && d(5) &&
        d(6) && have(7) && q[7] == '-' && d(8) && d(9)) {
      size_t len = 10;
      // Optional time, separated by 'T'/'t' or a single space.
      bool sep = have(len) && (q[len] == 'T' || q[len] == 't' || q[len] == ' ');
      if (sep && have(len + 1) && digit(q[len + 1])) {
        size_t t = match_time(q + len + 1);
        if (t) len += 1 + t;
      }
      return len;
    }
    // Local time only: dd:dd:dd
    size_t t = match_time(q);
    return t;
  }

  // Match HH:MM:SS(.frac)?(Z|z|±HH:MM)? starting at `q`; return matched length.
  size_t match_time(const char* q) {
    auto have = [&](size_t k) { return q + k < end_; };
    auto d = [&](size_t k) { return have(k) && digit(q[k]); };
    if (!(d(0) && d(1) && have(2) && q[2] == ':' && d(3) && d(4) && have(5) &&
          q[5] == ':' && d(6) && d(7)))
      return 0;
    size_t len = 8;
    if (have(len) && q[len] == '.') {  // fractional seconds
      size_t f = len + 1;
      while (q + f < end_ && digit(q[f])) f++;
      if (f > len + 1) len = f;
    }
    if (have(len) && (q[len] == 'Z' || q[len] == 'z')) {  // UTC
      len += 1;
    } else if (have(len) && (q[len] == '+' || q[len] == '-')) {  // ±HH:MM
      const char* o = q + len;
      if (o + 5 < end_ && digit(o[1]) && digit(o[2]) && o[3] == ':' &&
          digit(o[4]) && digit(o[5]))
        len += 6;
    }
    return len;
  }
};

inline Node parse(std::string_view text) {
  Parser parser(text.data(), text.data() + text.size());
  return parser.parse_document();
}

// --- serializer -----------------------------------------------------------

// Render a double as a valid TOML float (always carrying a '.' or exponent,
// and using inf/nan literals).
inline std::string float_to_toml(double f) {
  if (std::isnan(f)) return "nan";
  if (std::isinf(f)) return f < 0 ? "-inf" : "inf";
  char buf[32];
  auto* end =
      std::to_chars(buf, buf + sizeof(buf), f, std::chars_format::general).ptr;
  std::string s(buf, end);
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos)
    s += ".0";
  return s;
}

inline bool is_bare_key(const std::string& k) {
  if (k.empty()) return false;
  for (char c : k)
    if (!Parser::is_bare_key_char(c)) return false;
  return true;
}

inline std::string escape_basic(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\f': out += "\\f"; break;
      case '\r': out += "\\r"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char tmp[8];
          std::snprintf(tmp, sizeof(tmp), "\\u%04X", static_cast<unsigned char>(c));
          out += tmp;
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}

inline std::string render_key(const std::string& k) {
  return is_bare_key(k) ? k : escape_basic(k);
}

// Render a non-table value inline (scalars, arrays, inline tables).
inline std::string render_inline(const Node& n) {
  switch (n.kind) {
    case Kind::String: return escape_basic(n.s);
    case Kind::Int:    return std::to_string(n.i);
    case Kind::Float:  return float_to_toml(n.f);
    case Kind::Bool:   return n.b ? "true" : "false";
    case Kind::Array: {
      std::string out = "[";
      for (size_t k = 0; k < n.elems.size(); k++) {
        if (k) out += ", ";
        out += render_inline(n.elems[k]);
      }
      out += "]";
      return out;
    }
    case Kind::Table: {
      std::string out = "{ ";
      bool first = true;
      for (const auto& kv : n.items) {
        if (!first) out += ", ";
        first = false;
        out += render_key(kv.first) + " = " + render_inline(kv.second);
      }
      out += " }";
      return out;
    }
  }
  return "";
}

// True if a value is rendered as a `[section]` rather than inline.
inline bool is_section(const Node& n) { return n.kind == Kind::Table; }

// True if a value is an array whose every element is a table (=> `[[...]]`).
inline bool is_array_of_tables(const Node& n) {
  if (n.kind != Kind::Array || n.elems.empty()) return false;
  for (const auto& e : n.elems)
    if (e.kind != Kind::Table) return false;
  return true;
}

// Recursively emit `table` under the dotted-key prefix `path`. Scalars and
// inline values come first, then `[section]` sub-tables, then `[[arrays]]`,
// so every bare key precedes the header that would otherwise capture it.
inline void emit_table(std::string& out, const Node& table,
                       const std::string& path, bool sort_keys) {
  std::vector<const std::pair<std::string, Node>*> entries;
  entries.reserve(table.items.size());
  for (const auto& kv : table.items) entries.push_back(&kv);
  if (sort_keys) {
    std::sort(entries.begin(), entries.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });
  }
  // Pass 1: inline key = value pairs.
  for (const auto* kv : entries) {
    if (is_section(kv->second) || is_array_of_tables(kv->second)) continue;
    out += render_key(kv->first) + " = " + render_inline(kv->second) + "\n";
  }
  // Pass 2: [section] sub-tables.
  for (const auto* kv : entries) {
    if (!is_section(kv->second)) continue;
    std::string child = path.empty() ? render_key(kv->first)
                                      : path + "." + render_key(kv->first);
    out += "\n[" + child + "]\n";
    emit_table(out, kv->second, child, sort_keys);
  }
  // Pass 3: [[array of tables]].
  for (const auto* kv : entries) {
    if (!is_array_of_tables(kv->second)) continue;
    std::string child = path.empty() ? render_key(kv->first)
                                      : path + "." + render_key(kv->first);
    for (const auto& e : kv->second.elems) {
      out += "\n[[" + child + "]]\n";
      emit_table(out, e, child, sort_keys);
    }
  }
}

inline std::string stringify(const Node& root, bool sort_keys = false) {
  std::string out;
  emit_table(out, root, "", sort_keys);
  return out;
}

}  // namespace culebra::toml
