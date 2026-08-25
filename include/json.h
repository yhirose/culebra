#pragma once

// JSON parse / stringify core for the `JSON` stdlib namespace.
//
// Single logic source shared by the binding layer (stdlib_jit.h) and the
// JIT/AOT runtime helpers (stdlib_jit.h) so the three backends agree
// byte-for-byte on values, error kinds/messages, and 1-based error
// positions. Unlike toml.h there is no intermediate node tree: JSON.parse
// is hot, so the parser is templated on a small Builder policy that
// constructs each backend's values directly, and stringify on a Reader
// policy that inspects them.
//
// Builder requirements (see _JsonValueBuilder / _JitJsonBuilder):
//   using Value / Object / Array;
//   Value make_null() / boolean(bool) / integer(int64_t) / real(double) /
//         string(std::string&&);   // make_null, not nil: <objc> macro-stomps
//   Object object_new();  void object_set(Object&, const std::string& key,
//         Value&&);  Value object_done(Object&&);
//         void object_abandon(Object&);                        // throw path
//   Array array_new();  void array_push(Array&, Value&&);
//         Value array_done(Array&&);  void array_abandon(Array&);
//   void abandon_value(Value&);   // release a finished value on a throw path
//
// A partially-built container (or a finished root value that a later
// trailing-input check rejects) must be released if parsing throws.
// Backends over value types (interp ObjectValue/ArrayValue/Value) get this
// from C++ stack unwinding, so their *_abandon / abandon_value are no-ops;
// backends over raw refcounted heap objects (JIT JitObject*/JitArray*/
// JitValue) release the +1 there.
//
// Reader requirements (see _JsonValueReader / _JitJsonReader):
//   using Value;
//   Kind kind(v);  bool as_bool(v);  int64_t as_long(v);
//   double as_double(v);  std::string_view as_string(v);
//   size_t seq_size(v);  const Value& seq_at(v, i);
//   bool object_has_non_string_keys(v);
//   std::vector<std::pair<std::string_view, const Value*>> object_entries(v);
//   std::string_view type_name(v);                            // error text
//
// Errors throw culebra::CulebraError directly (shared.h — which also owns
// the leaf formatters json_escape / format_float_shortest), so no rethrow
// seam exists between the core and either backend.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shared.h"

namespace culebra::json {

// Value classification for stringify. Array/Tuple/Set all render as JSON
// arrays (Seq); Other rejects with "cannot serialize <type>".
enum class Kind { Nil, Bool, Long, Float, String, Seq, Object, Other };

// Containers nested deeper than this are a ValueError instead of a C-stack
// overflow (machine-written input: 100k `[`s). Applies to parse and to the
// value tree stringify walks.
inline constexpr int64_t kJsonDepthLimit = kCulebraRecursionLimit;

// Minimal recursive-descent JSON parser. Tracks 1-based line/col so
// `JSON.parse(bad)` surfaces `e.line` / `e.col` pointing at the offending
// character.
template <class B>
struct Parser {
  using V = typename B::Value;

  const char* p;
  const char* end;
  int64_t line = 1;
  int64_t col = 1;
  // 'auto' (default): integer-shaped → Long, otherwise Float.
  // 'float'         : every number → Float.
  std::string_view number_mode = "auto";
  // JSONC tolerance: `//` and `/* */` comments + trailing commas. Off by
  // default so strict JSON stays strict (an unexpected `//` is an error).
  bool jsonc = false;

  void advance() {
    if (*p == '\n') { line++; col = 1; }
    else            { col++; }
    ++p;
  }
  void skip_ws() {
    while (p < end) {
      char c = *p;
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { advance(); continue; }
      if (jsonc && c == '/' && p + 1 < end) {
        if (p[1] == '/') {                 // line comment → to end of line
          advance(); advance();
          while (p < end && *p != '\n') advance();
          continue;
        }
        if (p[1] == '*') {                 // block comment → to closing */
          advance(); advance();
          while (p < end && !(*p == '*' && p + 1 < end && p[1] == '/')) advance();
          if (p < end) { advance(); advance(); }
          continue;
        }
      }
      break;
    }
  }
  [[noreturn]] void fail(const char* msg) {
    // Structured line/col fields (not appended text) so eval()'s catch
    // doesn't replace our JSON-internal location with the caller AST's.
    throw CulebraError("ValueError",
                       std::format("JSON.parse: {}", msg), line, col);
  }
  // `depth` is the container nesting level of the value being parsed; the
  // container parsers reject level kJsonDepthLimit so the C stack stays
  // bounded on adversarial input.
  V parse_value(int64_t depth) {
    skip_ws();
    if (p >= end) fail("unexpected end");
    char c = *p;
    if (c == '{') return parse_object(depth);
    if (c == '[') return parse_array(depth);
    if (c == '"') return parse_string();
    if (c == 't' || c == 'f') return parse_bool();
    if (c == 'n') return parse_null();
    return parse_number();
  }
  void check_depth(int64_t depth) {
    if (depth >= kJsonDepthLimit) {
      fail(nesting_too_deep_message(kJsonDepthLimit).c_str());
    }
  }
  V parse_object(int64_t depth) {
    check_depth(depth);
    advance(); skip_ws();
    auto obj = B::object_new();
    // Any throw below (malformed input or a nested parse) must release the
    // partially-built object — see the ownership note at the top of the file.
    try {
      if (p < end && *p == '}') { advance(); return B::object_done(std::move(obj)); }
      while (p < end) {
        skip_ws();
        // JSONC: a `,` may be followed by `}` (trailing comma).
        if (jsonc && p < end && *p == '}') {
          advance();
          return B::object_done(std::move(obj));
        }
        auto key = parse_string_raw();
        skip_ws();
        if (p >= end || *p != ':') fail("expected ':'");
        advance();
        B::object_set(obj, key, parse_value(depth + 1));
        skip_ws();
        if (p < end && *p == ',') { advance(); continue; }
        if (p < end && *p == '}') { advance(); return B::object_done(std::move(obj)); }
        fail("expected ',' or '}'");
      }
      fail("unterminated object");
    } catch (...) {
      B::object_abandon(obj);
      throw;
    }
  }
  V parse_array(int64_t depth) {
    check_depth(depth);
    advance(); skip_ws();
    auto arr = B::array_new();
    try {
      if (p < end && *p == ']') { advance(); return B::array_done(std::move(arr)); }
      while (p < end) {
        skip_ws();
        // JSONC: a `,` may be followed by `]` (trailing comma).
        if (jsonc && p < end && *p == ']') {
          advance();
          return B::array_done(std::move(arr));
        }
        B::array_push(arr, parse_value(depth + 1));
        skip_ws();
        if (p < end && *p == ',') { advance(); continue; }
        if (p < end && *p == ']') { advance(); return B::array_done(std::move(arr)); }
        fail("expected ',' or ']'");
      }
      fail("unterminated array");
    } catch (...) {
      B::array_abandon(arr);
      throw;
    }
  }
  V parse_string() { return B::string(parse_string_raw()); }
  std::string parse_string_raw() {
    if (*p != '"') fail("expected string");
    advance();
    std::string out;
    while (p < end && *p != '"') {
      if (*p == '\\' && p + 1 < end) {
        advance();
        switch (*p) {
          case '"':  out += '"'; break;
          case '\\': out += '\\'; break;
          case '/':  out += '/'; break;
          case 'n':  out += '\n'; break;
          case 'r':  out += '\r'; break;
          case 't':  out += '\t'; break;
          case 'b':  out += '\b'; break;
          case 'f':  out += '\f'; break;
          default:   fail("bad escape");
        }
        advance();
      } else {
        out += *p; advance();
      }
    }
    if (p >= end) fail("unterminated string");
    advance();
    return out;
  }
  V parse_bool() {
    if (end - p >= 4 && std::string_view(p, 4) == "true") {
      for (int i = 0; i < 4; i++) advance();
      return B::boolean(true);
    }
    if (end - p >= 5 && std::string_view(p, 5) == "false") {
      for (int i = 0; i < 5; i++) advance();
      return B::boolean(false);
    }
    fail("bad bool");
  }
  V parse_null() {
    if (end - p >= 4 && std::string_view(p, 4) == "null") {
      for (int i = 0; i < 4; i++) advance();
      return B::make_null();
    }
    fail("bad null");
  }
  V parse_number() {
    const char* start = p;
    if (*p == '-') advance();
    bool is_float = false;
    const char* digits_start = p;
    while (p < end && (*p >= '0' && *p <= '9')) advance();
    if (p == digits_start) fail("expected value");
    if (p < end && *p == '.') {
      is_float = true; advance();
      while (p < end && (*p >= '0' && *p <= '9')) advance();
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
      is_float = true; advance();
      if (p < end && (*p == '+' || *p == '-')) advance();
      while (p < end && (*p >= '0' && *p <= '9')) advance();
    }
    std::string buf(start, p);
    if (number_mode == "float" || is_float) {
      return B::real(std::stod(buf));
    }
    return B::integer(static_cast<int64_t>(std::stoll(buf)));
  }
};

// Parse a complete JSON document (trailing non-whitespace is an error).
template <class B>
typename B::Value parse(std::string_view s, std::string_view number_mode,
                        bool jsonc) {
  Parser<B> jp{s.data(), s.data() + s.size()};
  jp.number_mode = number_mode;
  jp.jsonc = jsonc;
  auto v = jp.parse_value(0);
  jp.skip_ws();
  if (jp.p != jp.end) {
    B::abandon_value(v);  // v is a finished +1; release before the throw
    throw CulebraError("ValueError",
                       "JSON.parse: trailing characters", jp.line, jp.col);
  }
  return v;
}

// JSON Lines: split `s` on `\n`, parse each non-empty line as its own JSON
// value, return an Array. The outer line counter advances with each `\n` so
// error positions stay coherent across the whole input.
template <class B>
typename B::Value parse_lines(std::string_view s,
                              std::string_view number_mode, bool jsonc) {
  auto arr = B::array_new();
  try {
    long lineno = 1;
    size_t i = 0;
    while (i < s.size()) {
      size_t j = s.find('\n', i);
      auto slice =
          s.substr(i, j == std::string_view::npos ? s.size() - i : j - i);
      if (!slice.empty()) {
        Parser<B> jp{slice.data(), slice.data() + slice.size()};
        jp.line = lineno;
        jp.number_mode = number_mode;
        jp.jsonc = jsonc;
        B::array_push(arr, jp.parse_value(0));
        jp.skip_ws();
        if (jp.p != jp.end) {
          throw CulebraError("ValueError",
                             "JSON.parse(lines: true): trailing characters",
                             jp.line, jp.col);
        }
      }
      if (j == std::string_view::npos) break;
      i = j + 1;
      lineno++;
    }
  } catch (...) {
    B::array_abandon(arr);  // release the partial array on the throw path
    throw;
  }
  return B::array_done(std::move(arr));
}

// `indent` > 0 pretty-prints with that many spaces per level; 0 is compact.
// `sort_keys` walks Object keys alphabetically (deterministic output for
// diffs / hashing). `depth` tracks recursion for indentation and for the
// kJsonDepthLimit guard (a loop can build a value deeper than any parse).
template <class R>
std::string stringify(const typename R::Value& v, int indent = 0,
                      bool sort_keys = false, int depth = 0) {
  auto sep = [&](int level) -> std::string {
    if (indent <= 0) return "";
    return std::string("\n") + std::string(indent * level, ' ');
  };
  auto kind = R::kind(v);
  if (depth >= kJsonDepthLimit && (kind == Kind::Seq || kind == Kind::Object)) {
    throw CulebraError("ValueError",
        "JSON.stringify: " + nesting_too_deep_message(kJsonDepthLimit));
  }
  switch (kind) {
    case Kind::Nil:  return "null";
    case Kind::Bool: return R::as_bool(v) ? "true" : "false";
    case Kind::Long: return std::to_string(R::as_long(v));
    case Kind::Float: {
      double d = R::as_double(v);
      if (!std::isfinite(d)) {
        throw CulebraError("ValueError", "JSON.stringify: non-finite Float");
      }
      return format_float_shortest(d);
    }
    case Kind::String: return json_escape(R::as_string(v));
    case Kind::Seq: {
      // Array, Tuple, and Set all render as JSON arrays (Set in insertion
      // order).
      size_t n = R::seq_size(v);
      if (n == 0) return "[]";
      std::string s = "[";
      for (size_t i = 0; i < n; i++) {
        if (i) s += ",";
        s += sep(depth + 1);
        s += stringify<R>(R::seq_at(v, i), indent, sort_keys, depth + 1);
      }
      s += sep(depth);
      return s + "]";
    }
    case Kind::Object: {
      // Reject Objects carrying non-String keys (Long/Tuple/etc.) so
      // stringify ↔ parse stays a clean round trip.
      if (R::object_has_non_string_keys(v)) {
        throw CulebraError("TypeError",
                           "JSON.stringify: Object has non-String keys");
      }
      auto entries = R::object_entries(v);
      if (entries.empty()) return "{}";
      const char* colon = indent > 0 ? ": " : ":";
      if (sort_keys) {
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
      }
      std::string s = "{";
      bool first = true;
      for (const auto& [k, val_ptr] : entries) {
        if (!first) s += ",";
        first = false;
        s += sep(depth + 1);
        s += json_escape(k) + colon +
             stringify<R>(*val_ptr, indent, sort_keys, depth + 1);
      }
      s += sep(depth);
      return s + "}";
    }
    case Kind::Other: break;
  }
  throw CulebraError("TypeError", std::format(
      "JSON.stringify: cannot serialize {}", R::type_name(v)));
}

// JSON Lines: each element on its own compact line, terminated by `\n`.
// `v` must be an Array/Tuple/Set; the `indent > 0` incompatibility is
// rejected by the caller before it gets here.
template <class R>
std::string stringify_lines(const typename R::Value& v,
                            bool sort_keys = false) {
  if (R::kind(v) != Kind::Seq) {
    throw CulebraError("TypeError", std::format(
        "JSON.stringify(lines: true): expects Array/Tuple/Set, got {}",
        R::type_name(v)));
  }
  std::string s;
  size_t n = R::seq_size(v);
  for (size_t i = 0; i < n; i++) {
    s += stringify<R>(R::seq_at(v, i), /*indent=*/0, sort_keys);
    s += '\n';
  }
  return s;
}

}  // namespace culebra::json
