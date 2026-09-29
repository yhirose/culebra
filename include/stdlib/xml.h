#pragma once

// XML core for the `XML` stdlib namespace: a pull parser, the path language,
// and stringify.
//
// Value-neutral like json.h: the parser yields plain events, XML.parse builds
// its tree through a Builder policy, and the path evaluator and stringify read
// values through a Reader policy, so bindings.h's JitValue adapters are the
// only backend-specific code and every lane runs this one implementation.
//
// Builder requirements (see _JitXmlBuilder); parse calls them on the one
// builder object it is given, so per-parse state lives there:
//   using Value; using Elem;
//   Elem element_new(std::string_view tag, bool has_ns,
//                    const std::string& ns,
//                    const std::vector<Attr>& attrs);            // +1
//   void append_element(Elem& parent, Elem& child);  // parent takes the +1
//   void append_text(Elem& parent, std::string_view text);
//   Value element_done(Elem& root);  void element_abandon(Elem& root);
//
// Reader requirements (see _JitXmlReader):
//   using Value;
//   VKind kind(v);  std::string_view as_string(v);
//   std::string scalar_text(v);                       // Long / Float / Bool
//   const Value* field(v, name);                      // Object member or null
//   size_t array_size(v);  const Value& array_at(v, i);
//   std::vector<std::pair<std::string_view, const Value*>> object_entries(v);
//   std::string_view type_name(v);                    // error text
//
// Errors throw culebra::CulebraError. Parse errors carry the document's
// 1-based line/col; path and stringify errors carry none, so the call site
// is reported.

#include <base/format.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/shared.h"

namespace culebra::xml {

// Elements nested deeper than this are a ValueError instead of a C-stack
// overflow, in parse and in the trees find/stringify walk (like JSON).
inline constexpr int64_t kXmlDepthLimit = kCulebraRecursionLimit;

inline constexpr std::string_view kXmlNamespace =
    "http://www.w3.org/XML/1998/namespace";
inline constexpr std::string_view kXmlnsNamespace =
    "http://www.w3.org/2000/xmlns/";

struct Attr {
  std::string name;
  std::string value;
};

enum class EventKind { Start, End, Text };

struct Event {
  EventKind kind = EventKind::Text;
  std::string tag;             // Start / End
  bool has_ns = false;         // Start / End
  std::string ns;
  std::vector<Attr> attrs;     // Start
  std::string text;            // Text
};

inline bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
// XML 1.0 (Fifth Edition) [4] NameStartChar and [4a] NameChar.
inline bool is_name_start_cp(char32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         c == ':' || (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xF6) ||
         (c >= 0xF8 && c <= 0x2FF) || (c >= 0x370 && c <= 0x37D) ||
         (c >= 0x37F && c <= 0x1FFF) || (c >= 0x200C && c <= 0x200D) ||
         (c >= 0x2070 && c <= 0x218F) || (c >= 0x2C00 && c <= 0x2FEF) ||
         (c >= 0x3001 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFDCF) ||
         (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}
inline bool is_name_cp(char32_t c) {
  return is_name_start_cp(c) || c == '-' || c == '.' ||
         (c >= '0' && c <= '9') || c == 0xB7 || (c >= 0x300 && c <= 0x36F) ||
         (c >= 0x203F && c <= 0x2040);
}
// Byte length of the name character at `s[i]` (a NameStartChar when
// `start`), or 0 when there is none: bytes that are not UTF-8 name nothing.
inline size_t name_char_len(std::string_view s, size_t i, bool start) {
  if (i >= s.size()) return 0;
  char32_t cp;
  size_t n = unicode::utf8::decode_codepoint(s.data() + i, s.size() - i, cp);
  if (n == 0) return 0;
  return (start ? is_name_start_cp(cp) : is_name_cp(cp)) ? n : 0;
}
// The end of the run of name characters that starts at `s[i]`.
inline size_t name_chars_end(std::string_view s, size_t i) {
  while (size_t n = name_char_len(s, i, false)) i += n;
  return i;
}
// A QName (Namespaces in XML 1.0 [7]): a Name with at most one ':', and a
// NameStartChar on each side of it. The parser and stringify both hold
// element and attribute names to it.
inline bool is_qname(std::string_view s) {
  if (s.empty() || s[0] == ':' || !name_char_len(s, 0, true) ||
      name_chars_end(s, 0) != s.size()) {
    return false;
  }
  auto c = s.find(':');
  return c == std::string_view::npos ||
         (s.find(':', c + 1) == std::string_view::npos &&
          name_char_len(s, c + 1, true));
}
inline bool is_xml_char(uint32_t cp) {
  return cp == 0x9 || cp == 0xA || cp == 0xD ||
         (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD) ||
         (cp >= 0x10000 && cp <= 0x10FFFF);
}

[[noreturn]] inline void throw_too_deep(std::string_view fn) {
  throw CulebraError("ValueError", culebra::format(
      "{}: {}", fn, nesting_too_deep_message(kXmlDepthLimit)));
}

// The pull parser. `next` yields Start / End / Text events in document order
// and false once the document is complete; a malformed document throws when
// the scan reaches the fault, so a lazy consumer sees every event before it.
class PullParser {
 public:
  PullParser(std::string_view src, bool keep_space, std::string fn)
      : p_(src.data()), end_(src.data() + src.size()),
        keep_space_(keep_space), fn_(std::move(fn)) {}

  bool next(Event& ev) {
    if (pending_end_) {
      pending_end_ = false;
      emit_end(ev);
      return true;
    }
    if (done_) return false;
    if (!started_) {
      started_ = true;
      read_prolog_start();
    }
    for (;;) {
      if (stack_.empty()) {
        if (!skip_misc()) {
          done_ = true;
          return false;
        }
        start_tag(ev);
        return true;
      }
      if (p_ >= end_) {
        fail(culebra::format("unexpected end of input: '<{}>' is not closed",
                             stack_.back().tag));
      }
      char c = *p_;
      if (c == '<') {
        if (at("<!--")) { skip_comment(); continue; }
        if (at("<![CDATA[")) { read_cdata(); continue; }
        if (at("<?")) { skip_pi(); continue; }
        if (at("<!")) fail("unexpected '<!' in element content");
        if (flush_text(ev)) return true;
        if (at("</")) end_tag(ev);
        else start_tag(ev);
        return true;
      }
      if (c == '&') { read_reference(text_); continue; }
      if (c == ']' && at("]]>")) fail("']]>' is not allowed in text");
      if (c == '\r') {  // line ends read as '\n' (XML 1.0 section 2.11)
        skip_cr();
        text_ += '\n';
        continue;
      }
      text_ += c;
      advance();
    }
  }

 private:
  struct Open {
    std::string tag;
    bool has_ns;
    std::string ns;
    size_t scope_mark;
  };
  struct Binding {
    std::string prefix;  // "" = the default namespace
    std::string uri;     // "" = undeclared (xmlns="")
  };

  const char* p_;
  const char* end_;
  const char* valid_until_ = nullptr;
  int64_t line_ = 1;
  int64_t col_ = 1;
  bool keep_space_;
  std::string fn_;
  bool started_ = false;
  bool root_seen_ = false;
  bool doctype_seen_ = false;
  bool pending_end_ = false;
  bool done_ = false;
  std::vector<Open> stack_;
  std::vector<Binding> scope_;
  std::string text_;
  // Per start tag, kept to reuse their storage: each attribute's position,
  // and the namespaced attributes resolved so far (index, URI).
  std::vector<std::pair<int64_t, int64_t>> attr_pos_;
  std::vector<std::pair<size_t, const std::string*>> ns_attrs_;

  [[noreturn]] void fail_at(const std::string& msg, int64_t line,
                            int64_t col) {
    throw CulebraError("ValueError", culebra::format("{}: {}", fn_, msg), line,
                       col);
  }
  [[noreturn]] void fail(const std::string& msg) { fail_at(msg, line_, col_); }

  // Every byte is consumed here, so each character is checked once as the
  // scan reaches it: invalid UTF-8 and non-XML characters fail in place.
  void advance() {
    auto b = static_cast<unsigned char>(*p_);
    // Printable ASCII is always one whole, allowed character.
    if ((b < 0x20 || b > 0x7E) && p_ >= valid_until_) check_char();
    if (*p_ == '\n') { line_++; col_ = 1; }
    else             { col_++; }
    ++p_;
  }
  void check_char() {
    char32_t cp;
    size_t n = unicode::utf8::decode_codepoint(
        p_, static_cast<size_t>(end_ - p_), cp);
    if (n == 0) fail("invalid UTF-8");
    if (!is_xml_char(cp)) {
      fail(culebra::format("invalid character U+{:04X}",
                           static_cast<uint32_t>(cp)));
    }
    valid_until_ = p_ + n;
  }
  void advance_n(size_t n) { while (n--) advance(); }
  // At '\r': consume it and a '\n' after it, the one line end they spell.
  void skip_cr() {
    advance();
    if (p_ < end_ && *p_ == '\n') advance();
  }
  std::string_view rest() const {
    return std::string_view(p_, static_cast<size_t>(end_ - p_));
  }
  bool at(std::string_view s) const {
    return static_cast<size_t>(end_ - p_) >= s.size() &&
           std::string_view(p_, s.size()) == s;
  }
  bool skip_ws() {
    bool any = false;
    while (p_ < end_ && is_space(*p_)) { advance(); any = true; }
    return any;
  }
  std::string read_name(const char* what) {
    if (!name_char_len(rest(), 0, true)) {
      fail(culebra::format("expected {}", what));
    }
    std::string name(p_, name_chars_end(rest(), 0));
    advance_n(name.size());
    return name;
  }
  void check_qname(const std::string& name, int64_t line, int64_t col) {
    if (!is_qname(name)) {
      fail_at(culebra::format("invalid qualified name '{}'", name), line, col);
    }
  }

  void read_prolog_start() {
    if (at("\xEF\xBB\xBF")) {  // a UTF-8 byte order mark is not content
      p_ += 3;
      valid_until_ = p_;
    }
    if (at("<?xml") && static_cast<size_t>(end_ - p_) > 5 &&
        (is_space(p_[5]) || p_[5] == '?')) {
      read_xml_decl();
    }
  }
  // XML 1.0 [23]-[32]: `version`, then optional `encoding`, then optional
  // `standalone`, each once and each after whitespace.
  void read_xml_decl() {
    static constexpr std::string_view kOrder[] = {"version", "encoding",
                                                  "standalone"};
    advance_n(5);
    size_t next = 0;
    for (;;) {
      bool ws = skip_ws();
      if (p_ >= end_) fail("unterminated XML declaration");
      if (at("?>")) break;
      if (!ws) fail("expected whitespace in the XML declaration");
      int64_t nline = line_, ncol = col_;
      auto name = read_name("a name in the XML declaration");
      size_t k = std::find(std::begin(kOrder), std::end(kOrder), name) -
                 std::begin(kOrder);
      if (k == std::size(kOrder)) {
        fail_at(culebra::format("unexpected '{}' in the XML declaration", name),
                nline, ncol);
      }
      if (next == 0 && k != 0) {
        fail_at("the XML declaration must start with version", nline, ncol);
      }
      if (k < next) {
        fail_at(culebra::format(
                    "'{}' is repeated or out of order in the XML declaration",
                    name),
                nline, ncol);
      }
      next = k + 1;
      int64_t vline, vcol;
      auto value = read_decl_value(vline, vcol);
      if (k == 0 && !(value.size() > 2 && value.starts_with("1.") &&
                      std::all_of(value.begin() + 2, value.end(), [](char c) {
                        return c >= '0' && c <= '9';
                      }))) {
        fail_at(culebra::format("version '{}' is not an XML 1.x version", value),
                vline, vcol);
      }
      if (k == 1 && ascii_lower(value) != "utf-8") {
        fail_at(culebra::format("encoding '{}' is not supported (only UTF-8)",
                                value),
                vline, vcol);
      }
      if (k == 2 && value != "yes" && value != "no") {
        fail_at(culebra::format("standalone must be 'yes' or 'no', got '{}'",
                                value),
                vline, vcol);
      }
    }
    if (next == 0) fail("the XML declaration has no version");
    advance_n(2);
  }
  // `= "value"` in the XML declaration; `line` / `col` get the value's start.
  std::string read_decl_value(int64_t& line, int64_t& col) {
    skip_ws();
    if (p_ >= end_ || *p_ != '=') fail("expected '=' in the XML declaration");
    advance();
    skip_ws();
    if (p_ >= end_ || (*p_ != '"' && *p_ != '\'')) {
      fail("expected a quoted value in the XML declaration");
    }
    char q = *p_;
    advance();
    line = line_;
    col = col_;
    const char* s = p_;
    while (p_ < end_ && *p_ != q) advance();
    if (p_ >= end_) fail("unterminated XML declaration");
    std::string value(s, p_);
    advance();
    return value;
  }

  // Whitespace, comments and PIs around the root (and the DOCTYPE before it).
  // True when positioned at the root's start tag, false at the end of input.
  bool skip_misc() {
    for (;;) {
      skip_ws();
      if (p_ >= end_) {
        if (!root_seen_) fail("no root element");
        return false;
      }
      if (at("<!--")) { skip_comment(); continue; }
      if (at("<?")) { skip_pi(); continue; }
      if (at("<!DOCTYPE")) {
        if (root_seen_ || doctype_seen_) fail("unexpected DOCTYPE");
        skip_doctype();
        continue;
      }
      if (*p_ == '<' && name_char_len(rest(), 1, true)) {
        if (root_seen_) fail("more than one root element");
        return true;
      }
      fail(root_seen_ ? "content after the root element"
                      : "content before the root element");
    }
  }

  void skip_comment() {
    int64_t line0 = line_, col0 = col_;
    advance_n(4);
    for (;;) {
      if (p_ >= end_) fail_at("unterminated comment", line0, col0);
      if (at("--")) {
        if (!at("-->")) fail("'--' is not allowed in a comment");
        advance_n(3);
        return;
      }
      advance();
    }
  }
  void skip_pi() {
    int64_t line0 = line_, col0 = col_;
    advance_n(2);
    int64_t tline = line_, tcol = col_;
    auto target = read_name("a processing instruction target");
    if (ascii_lower(target) == "xml") {
      fail_at("the XML declaration is only allowed at the start", line0, col0);
    }
    if (target.find(':') != std::string::npos) {  // Namespaces in XML 1.0 §7
      fail_at("a processing instruction target cannot contain ':'", tline,
              tcol);
    }
    if (p_ < end_ && !is_space(*p_) && !at("?>")) {
      fail("expected whitespace or '?>' after a processing instruction target");
    }
    for (;;) {
      if (p_ >= end_) fail_at("unterminated processing instruction", line0, col0);
      if (at("?>")) { advance_n(2); return; }
      advance();
    }
  }
  // Read past the DOCTYPE, internal subset included. Nothing it declares is
  // used: no entity is ever expanded (no XXE, no billion laughs).
  void skip_doctype() {
    int64_t line0 = line_, col0 = col_;
    doctype_seen_ = true;
    advance_n(9);
    if (p_ < end_ && !is_space(*p_)) fail("expected whitespace after '<!DOCTYPE'");
    char quote = 0;
    int depth = 0;
    while (p_ < end_) {
      char c = *p_;
      if (quote) {
        if (c == quote) quote = 0;
        advance();
        continue;
      }
      if (depth > 0 && at("<!--")) { skip_comment(); continue; }
      if (depth > 0 && at("<?")) { skip_pi(); continue; }
      if (c == '"' || c == '\'') quote = c;
      else if (c == '[') depth++;
      else if (c == ']') depth--;
      else if (c == '>' && depth == 0) { advance(); return; }
      advance();
    }
    fail_at("unterminated DOCTYPE", line0, col0);
  }

  void read_cdata() {
    int64_t line0 = line_, col0 = col_;
    advance_n(9);
    for (;;) {
      if (p_ >= end_) fail_at("unterminated CDATA section", line0, col0);
      if (at("]]>")) { advance_n(3); return; }
      if (*p_ == '\r') {
        skip_cr();
        text_ += '\n';
        continue;
      }
      text_ += *p_;
      advance();
    }
  }

  // `&...;` at p_: the five predefined entities and character references.
  void read_reference(std::string& out) {
    int64_t line0 = line_, col0 = col_;
    const char* start = p_;
    advance();
    if (p_ < end_ && *p_ == '#') {
      advance();
      bool hex = p_ < end_ && *p_ == 'x';
      if (hex) advance();
      uint32_t cp = 0;
      bool any = false;
      while (p_ < end_ && *p_ != ';') {
        char c = *p_;
        int d = hex ? hex_digit(c) : (c >= '0' && c <= '9' ? c - '0' : -1);
        if (d < 0) fail_at("malformed character reference", line0, col0);
        cp = cp > 0x10FFFF ? cp : cp * (hex ? 16 : 10) + static_cast<uint32_t>(d);
        any = true;
        advance();
      }
      if (!any || p_ >= end_) fail_at("malformed character reference", line0, col0);
      advance();  // ';'
      if (!is_xml_char(cp)) {
        fail_at(culebra::format(
                    "character reference '{}' is not a valid XML character",
                    std::string_view(start, p_)),
                line0, col0);
      }
      append_utf8(out, cp);
      return;
    }
    if (!name_char_len(rest(), 0, true)) {
      fail_at("'&' does not start an entity reference", line0, col0);
    }
    std::string_view name(p_, name_chars_end(rest(), 0));
    advance_n(name.size());
    if (p_ >= end_ || *p_ != ';') {
      fail_at("unterminated entity reference", line0, col0);
    }
    advance();
    if (name == "lt") out += '<';
    else if (name == "gt") out += '>';
    else if (name == "amp") out += '&';
    else if (name == "quot") out += '"';
    else if (name == "apos") out += '\'';
    else fail_at(culebra::format("undefined entity '&{};'", name), line0, col0);
  }

  // Attribute-value normalization (XML 1.0 section 3.3.3): each literal
  // whitespace character reads as a space, so stringify writes them as
  // character references to keep them.
  std::string read_att_value() {
    char q = *p_;
    int64_t line0 = line_, col0 = col_;
    advance();
    std::string out;
    for (;;) {
      if (p_ >= end_) fail_at("unterminated attribute value", line0, col0);
      char c = *p_;
      if (c == q) { advance(); return out; }
      if (c == '<') fail("'<' is not allowed in an attribute value");
      if (c == '&') { read_reference(out); continue; }
      if (c == '\r') {
        skip_cr();
        out += ' ';
        continue;
      }
      out += (c == '\n' || c == '\t') ? ' ' : c;
      advance();
    }
  }

  // The URI `prefix` is bound to, or null when nothing binds it. The default
  // namespace (prefix "") is null when undeclared.
  const std::string* bound(std::string_view prefix) const {
    static const std::string kXml(kXmlNamespace);
    if (prefix == "xml") return &kXml;
    for (auto it = scope_.rbegin(); it != scope_.rend(); ++it) {
      if (it->prefix == prefix) return it->uri.empty() ? nullptr : &it->uri;
    }
    return nullptr;
  }

  // Push the element's `xmlns` / `xmlns:p` declarations onto the scope,
  // holding them to the reserved names (Namespaces in XML 1.0 §3).
  void declare_namespaces(const std::vector<Attr>& attrs) {
    for (size_t i = 0; i < attrs.size(); i++) {
      const auto& a = attrs[i];
      bool is_default = a.name == "xmlns";
      if (!is_default && !a.name.starts_with("xmlns:")) continue;
      auto [aline, acol] = attr_pos_[i];
      std::string_view prefix =
          is_default ? std::string_view() : std::string_view(a.name).substr(6);
      if (prefix == "xmlns") {
        fail_at("the 'xmlns' prefix cannot be declared", aline, acol);
      }
      if (a.value == kXmlnsNamespace) {
        fail_at(culebra::format("the namespace '{}' cannot be declared",
                                kXmlnsNamespace),
                aline, acol);
      }
      if (prefix == "xml" && a.value != kXmlNamespace) {
        fail_at("the 'xml' prefix cannot be rebound", aline, acol);
      }
      if (prefix != "xml" && a.value == kXmlNamespace) {
        fail_at(culebra::format(
                    "the namespace '{}' is bound only to the 'xml' prefix",
                    kXmlNamespace),
                aline, acol);
      }
      if (prefix == "xml") continue;
      if (!is_default && a.value.empty()) {
        fail_at(culebra::format("namespace prefix '{}' cannot be undeclared",
                                prefix),
                aline, acol);
      }
      scope_.push_back({std::string(prefix), a.value});
    }
  }

  // The element's namespace into `uri` (false: none), after checking that
  // every prefix on it and its attributes is bound and that no two of its
  // attributes share a namespace and local name (Namespaces in XML 1.0 §6.3).
  bool resolve_names(const std::string& tag, int64_t line, int64_t col,
                     const std::vector<Attr>& attrs, std::string& uri) {
    const std::string* own;
    auto colon = tag.find(':');
    if (colon == std::string::npos) {
      own = bound("");
    } else {
      std::string_view prefix(tag.data(), colon);
      if (prefix == "xmlns") {
        fail_at("an element name cannot use the 'xmlns' prefix", line, col);
      }
      own = bound(prefix);
      if (!own) {
        fail_at(culebra::format("unbound namespace prefix '{}'", prefix), line,
                col);
      }
    }
    ns_attrs_.clear();
    for (size_t i = 0; i < attrs.size(); i++) {
      std::string_view n = attrs[i].name;
      auto c = n.find(':');
      if (c == std::string_view::npos) continue;
      std::string_view prefix = n.substr(0, c);
      if (prefix == "xmlns") continue;
      const std::string* u = bound(prefix);
      if (!u) {
        fail_at(culebra::format("unbound namespace prefix '{}'", prefix),
                attr_pos_[i].first, attr_pos_[i].second);
      }
      for (auto [j, uj] : ns_attrs_) {
        std::string_view m = attrs[j].name;
        if (*uj == *u && m.substr(m.find(':') + 1) == n.substr(c + 1)) {
          fail_at(culebra::format(
                      "attribute '{}' has the same namespace and local name "
                      "as '{}'",
                      n, m),
                  attr_pos_[i].first, attr_pos_[i].second);
        }
      }
      ns_attrs_.emplace_back(i, u);
    }
    if (own) uri = *own;
    return own != nullptr;
  }

  void start_tag(Event& ev) {
    int64_t line0 = line_, col0 = col_;
    if (static_cast<int64_t>(stack_.size()) >= kXmlDepthLimit) {
      fail(nesting_too_deep_message(kXmlDepthLimit));
    }
    advance();  // '<'
    auto tag = read_name("an element name");
    check_qname(tag, line0, col0 + 1);
    auto& attrs = ev.attrs;  // built in place: the event carries them out
    attrs.clear();
    attr_pos_.clear();
    bool self_closing;
    for (;;) {
      bool ws = skip_ws();
      if (p_ >= end_) fail("unexpected end of input in a start tag");
      if (*p_ == '>') { advance(); self_closing = false; break; }
      if (at("/>")) { advance_n(2); self_closing = true; break; }
      if (!ws) fail("expected whitespace, '>' or '/>' in a start tag");
      int64_t aline = line_, acol = col_;
      auto name = read_name("an attribute name");
      check_qname(name, aline, acol);
      skip_ws();
      if (p_ >= end_ || *p_ != '=') fail("expected '=' after an attribute name");
      advance();
      skip_ws();
      if (p_ >= end_ || (*p_ != '"' && *p_ != '\'')) {
        fail("attribute value must be quoted");
      }
      auto value = read_att_value();
      for (const auto& a : attrs) {
        if (a.name == name) {
          fail_at(culebra::format("duplicate attribute '{}'", name), aline, acol);
        }
      }
      attrs.push_back({std::move(name), std::move(value)});
      attr_pos_.emplace_back(aline, acol);
    }

    size_t mark = scope_.size();
    declare_namespaces(attrs);
    std::string uri;
    bool has_ns = resolve_names(tag, line0, col0 + 1, attrs, uri);

    root_seen_ = true;
    stack_.push_back({tag, has_ns, uri, mark});
    ev.kind = EventKind::Start;
    ev.tag = std::move(tag);
    ev.has_ns = has_ns;
    ev.ns = std::move(uri);
    ev.text.clear();
    pending_end_ = self_closing;
  }

  void end_tag(Event& ev) {
    int64_t line0 = line_, col0 = col_;
    advance_n(2);
    auto name = read_name("an element name");
    skip_ws();
    if (p_ >= end_ || *p_ != '>') fail("expected '>' to close an end tag");
    advance();
    if (name != stack_.back().tag) {
      fail_at(culebra::format("mismatched end tag: expected '</{}>', got '</{}>'",
                              stack_.back().tag, name),
              line0, col0);
    }
    emit_end(ev);
  }

  void emit_end(Event& ev) {
    auto& top = stack_.back();
    ev.kind = EventKind::End;
    ev.tag = std::move(top.tag);
    ev.has_ns = top.has_ns;
    ev.ns = std::move(top.ns);
    ev.attrs.clear();
    ev.text.clear();
    scope_.resize(top.scope_mark);
    stack_.pop_back();
  }

  bool flush_text(Event& ev) {
    if (text_.empty()) return false;
    if (!keep_space_ &&
        std::all_of(text_.begin(), text_.end(), [](char c) { return is_space(c); })) {
      text_.clear();
      return false;
    }
    ev.kind = EventKind::Text;
    ev.text.swap(text_);  // both buffers keep their capacity
    text_.clear();
    ev.tag.clear();
    ev.has_ns = false;
    ev.ns.clear();
    ev.attrs.clear();
    return true;
  }
};

// XML.parse: the root element, built from the pull parser's events.
template <class B>
typename B::Value parse(B& b, std::string_view src, bool keep_space,
                        std::string fn) {
  PullParser pp(src, keep_space, std::move(fn));
  Event ev;
  std::vector<typename B::Elem> stack;
  std::optional<typename B::Elem> root;
  try {
    while (pp.next(ev)) {
      switch (ev.kind) {
        case EventKind::Start: {
          auto el = b.element_new(ev.tag, ev.has_ns, ev.ns, ev.attrs);
          if (stack.empty()) root = el;
          else b.append_element(stack.back(), el);
          stack.push_back(el);
          break;
        }
        case EventKind::End:
          stack.pop_back();
          break;
        case EventKind::Text:
          b.append_text(stack.back(), ev.text);
          break;
      }
    }
  } catch (...) {
    if (root) b.element_abandon(*root);  // the root owns everything built
    throw;
  }
  return b.element_done(*root);
}

// --- Values seen through a Reader ---------------------------------------------

enum class VKind { Nil, String, Long, Float, Bool, Array, Object, Other };

// The element's `key` field (`tag`, `ns`) when it is a String.
template <class R>
std::optional<std::string_view> elem_string(const typename R::Value& el,
                                            std::string_view key) {
  const auto* t = R::field(el, key);
  if (!t || R::kind(*t) != VKind::String) return std::nullopt;
  return R::as_string(*t);
}
// The children Array, or null when the element has none.
template <class R>
const typename R::Value* elem_children(const typename R::Value& el) {
  const auto* c = R::field(el, "children");
  return c && R::kind(*c) == VKind::Array ? c : nullptr;
}
// The attrs Object, or null when the element has none.
template <class R>
const typename R::Value* elem_attrs(const typename R::Value& el) {
  const auto* a = R::field(el, "attrs");
  return a && R::kind(*a) == VKind::Object ? a : nullptr;
}
// The attribute `name`'s value, or null when the element has none.
template <class R>
const typename R::Value* elem_attr(const typename R::Value& el,
                                   std::string_view name) {
  const auto* a = elem_attrs<R>(el);
  return a ? R::field(*a, name) : nullptr;
}
// An attribute value as text: a String, Long, Float or Bool; anything else
// (only a hand-built Object has one) has none.
template <class R>
std::optional<std::string> attr_text(const typename R::Value& v) {
  switch (R::kind(v)) {
    case VKind::String: return std::string(R::as_string(v));
    case VKind::Long:
    case VKind::Float:
    case VKind::Bool: return R::scalar_text(v);
    default: return std::nullopt;
  }
}

// XPath string-value: every descendant text in document order. Iterative so
// a hand-built tree cannot overflow the C stack; the depth limit stops a
// cycle.
template <class R>
std::string string_value(const typename R::Value& el, std::string_view fn) {
  using V = typename R::Value;
  std::string out;
  struct Frame { const V* kids; size_t i; };
  std::vector<Frame> st;
  if (const auto* k = elem_children<R>(el)) st.push_back({k, 0});
  while (!st.empty()) {
    auto& f = st.back();
    if (f.i >= R::array_size(*f.kids)) { st.pop_back(); continue; }
    const V& c = R::array_at(*f.kids, f.i++);
    auto kind = R::kind(c);
    if (kind == VKind::String) {
      out += R::as_string(c);
    } else if (kind == VKind::Object) {
      if (const auto* k = elem_children<R>(c)) {
        if (static_cast<int64_t>(st.size()) >= kXmlDepthLimit) {
          throw_too_deep(fn);
        }
        st.push_back({k, 0});
      }
    }
  }
  return out;
}

// --- Paths --------------------------------------------------------------------

using NsMap = std::vector<std::pair<std::string, std::string>>;

struct NameTest {
  bool any_ns = false;
  bool has_ns = false;  // when !any_ns: false = no namespace
  std::string ns;
  bool any_local = false;
  std::string local;
};

struct Pred {
  enum Kind { HasAttr, AttrEq, HasChild, ChildEq, Pos, Last } kind;
  std::string attr;
  NameTest child;
  std::string lit;
  int64_t n = 0;  // Pos: the position; Last: the offset back from last()
};

struct Step {
  enum Kind { Self, Parent, Child } kind = Child;
  bool descendant = false;  // preceded by `//`
  NameTest test;
  std::vector<Pred> preds;
};

// Compiles the supported subset of XPath; anything else is a ValueError
// naming the construct and its 1-based column.
class PathCompiler {
 public:
  PathCompiler(std::string_view s, const NsMap& ns, std::string_view fn)
      : s_(s), ns_(ns), fn_(fn) {}

  std::vector<Step> compile() {
    std::vector<Step> steps;
    skip_ws();
    if (i_ >= s_.size()) fail("empty path");
    if (s_[i_] == '/') fail("a path is relative to the element");
    bool descendant = false;
    for (;;) {
      steps.push_back(step(descendant));
      skip_ws();
      if (i_ >= s_.size()) break;
      if (s_.substr(i_, 2) == "//") { descendant = true; i_ += 2; }
      else if (s_[i_] == '/') { descendant = false; i_ += 1; }
      else unsupported(i_);
      skip_ws();
      if (i_ >= s_.size()) unsupported(i_);
    }
    return steps;
  }

 private:
  std::string_view s_;
  const NsMap& ns_;
  std::string_view fn_;
  size_t i_ = 0;

  [[noreturn]] void fail(const std::string& msg) {
    throw CulebraError("ValueError", culebra::format("{}: {}", fn_, msg));
  }
  // Byte length of the NCName character at `k` (a start character when
  // `start`), or 0 when there is none: an NCName is a Name without ':'.
  size_t ncname_len(size_t k, bool start) const {
    if (k < s_.size() && s_[k] == ':') return 0;
    return name_char_len(s_, k, start);
  }
  size_t ncname_end(size_t k) const {
    while (size_t n = ncname_len(k, false)) k += n;
    return k;
  }

  // The construct at `k`, as the error names it: a word (with the `::` of an
  // axis or the `()` of a function call), a two-character operator, or one
  // character.
  std::string token_at(size_t k) const {
    if (ncname_len(k, true)) {
      size_t e = ncname_end(k);
      std::string w(s_.substr(k, e - k));
      size_t j = e;
      while (j < s_.size() && is_space(s_[j])) j++;
      if (s_.substr(j, 2) == "::") return w + "::";
      if (j < s_.size() && s_[j] == '(') return w + "()";
      return w;
    }
    for (std::string_view op : {"!=", "<=", ">=", "::", "//"}) {
      if (s_.substr(k, 2) == op) return std::string(op);
    }
    return std::string(s_.substr(k, utf8_scalar_len(s_, k)));
  }
  [[noreturn]] void unsupported(size_t k) {
    if (k >= s_.size()) {
      fail(culebra::format("unexpected end of path at column {}", k + 1));
    }
    fail(culebra::format("unsupported path syntax '{}' at column {}",
                         token_at(k), k + 1));
  }
  void skip_ws() {
    while (i_ < s_.size() && is_space(s_[i_])) i_++;
  }
  std::string ncname() {
    size_t b = i_;
    i_ = ncname_end(i_);
    return std::string(s_.substr(b, i_ - b));
  }
  // A word followed by `::` or `(` is an axis or a function: not a name.
  void reject_axis_or_call(size_t start) {
    size_t j = i_;
    while (j < s_.size() && is_space(s_[j])) j++;
    if (s_.substr(j, 2) == "::" || (j < s_.size() && s_[j] == '(')) {
      unsupported(start);
    }
  }
  // An NCName at i_ (unsupported syntax when there is none), refused when it
  // turns out to be an axis or a function call.
  std::string checked_ncname() {
    size_t start = i_;
    if (!ncname_len(i_, true)) unsupported(i_);
    auto w = ncname();
    reject_axis_or_call(start);
    return w;
  }

  NameTest name_test() {
    NameTest t;
    size_t start = i_;
    if (i_ >= s_.size()) unsupported(i_);
    if (s_[i_] == '*') {
      i_++;
      t.any_ns = true;
      if (i_ < s_.size() && s_[i_] == ':') {  // `*:tag`, XPath 2.0
        i_++;
        t.local = checked_ncname();
      } else {
        t.any_local = true;
      }
      return t;
    }
    if (s_[i_] == '{') brace_name(start);
    if (s_.substr(i_, 2) == "Q{") {  // `Q{uri}tag`, XPath 3.0's EQName
      auto close = s_.find('}', i_);
      if (close == std::string_view::npos) unsupported(start);
      std::string_view uri = s_.substr(i_ + 2, close - i_ - 2);
      // A BracedURILiteral holds no '{' (XPath 3.0 [118]).
      if (auto b = uri.find('{'); b != std::string_view::npos) {
        unsupported(i_ + 2 + b);
      }
      i_ = close + 1;
      if (!uri.empty()) {
        t.has_ns = true;
        t.ns = uri;
      }
      local_part(t);
      return t;
    }
    auto first = checked_ncname();
    if (i_ < s_.size() && s_[i_] == ':') {
      i_++;
      const std::string* uri = nullptr;
      for (const auto& [p, u] : ns_) {
        if (p == first) {
          uri = &u;
          break;
        }
      }
      if (!uri) {
        fail(culebra::format("unknown namespace prefix '{}' at column {}", first,
                             start + 1));
      }
      if (!uri->empty()) {
        t.has_ns = true;
        t.ns = *uri;
      }
      local_part(t);
      return t;
    }
    // Unprefixed: no namespace, unless `namespaces` maps "" (XPath 2.0's
    // default element namespace).
    for (const auto& [p, u] : ns_) {
      if (p.empty() && !u.empty()) {
        t.has_ns = true;
        t.ns = u;
      }
    }
    t.local = std::move(first);
    return t;
  }
  // `{uri}tag` is not XPath: name the spelling that is.
  [[noreturn]] void brace_name(size_t start) {
    auto close = s_.find('}', start);
    if (close == std::string_view::npos) unsupported(start);
    size_t e = close + 1;
    if (e < s_.size() && s_[e] == '*') {
      e++;
    } else {
      e = ncname_end(e);
    }
    std::string_view uri = s_.substr(start + 1, close - start - 1);
    std::string_view local = s_.substr(close + 1, e - close - 1);
    std::string hint = uri == "*" ? culebra::format("*:{}", local)
                                  : culebra::format("Q{{{}}}{}", uri, local);
    fail(culebra::format("'{}' is not XPath syntax at column {}; write {}",
                         s_.substr(start, e - start), start + 1, hint));
  }
  void local_part(NameTest& t) {
    if (i_ < s_.size() && s_[i_] == '*') {
      i_++;
      t.any_local = true;
      return;
    }
    t.local = checked_ncname();
  }

  Step step(bool descendant) {
    Step st;
    st.descendant = descendant;
    if (s_.substr(i_, 2) == "..") {
      i_ += 2;
      st.kind = Step::Parent;
    } else if (s_[i_] == '.') {
      i_ += 1;
      st.kind = Step::Self;
    } else {
      st.test = name_test();
      st.kind = Step::Child;
    }
    for (;;) {
      size_t save = i_;
      skip_ws();
      if (i_ >= s_.size() || s_[i_] != '[') {
        i_ = save;
        break;
      }
      // XPath gives the abbreviated `.` and `..` no predicates.
      if (st.kind != Step::Child) unsupported(i_);
      st.preds.push_back(pred());
    }
    return st;
  }

  std::string literal() {
    if (i_ >= s_.size() || (s_[i_] != '\'' && s_[i_] != '"')) unsupported(i_);
    char q = s_[i_];
    auto close = s_.find(q, i_ + 1);
    if (close == std::string_view::npos) unsupported(i_);
    std::string v(s_.substr(i_ + 1, close - i_ - 1));
    i_ = close + 1;
    return v;
  }
  int64_t integer() {
    if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') unsupported(i_);
    int64_t n = 0;
    while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
      if (n < (int64_t{1} << 40)) n = n * 10 + (s_[i_] - '0');
      i_++;
    }
    return n;
  }

  // `= 'lit'` after a predicate's name: true with the literal read into
  // `lit`, false when the name stands alone as a presence test.
  bool eq_literal(std::string& lit) {
    skip_ws();
    if (i_ >= s_.size() || s_[i_] != '=') return false;
    i_++;
    skip_ws();
    lit = literal();
    return true;
  }

  // `last` followed by `(`; a bare `last` is a child named last.
  bool is_last_call() const {
    if (s_.substr(i_, 4) != "last") return false;
    size_t j = i_ + 4;
    if (ncname_len(j, false)) return false;
    while (j < s_.size() && is_space(s_[j])) j++;
    return j < s_.size() && s_[j] == '(';
  }

  Pred pred() {
    size_t open = i_;
    i_++;  // '['
    skip_ws();
    if (i_ >= s_.size()) unsupported(i_);
    Pred p{};
    char c = s_[i_];
    if (c == '@') {
      i_++;
      size_t b = i_;
      checked_ncname();
      if (i_ < s_.size() && s_[i_] == ':') {  // matched as written: `@xlink:href`
        i_++;
        if (!ncname_len(i_, true)) unsupported(i_);
        ncname();
      }
      p.attr = std::string(s_.substr(b, i_ - b));
      p.kind = eq_literal(p.lit) ? Pred::AttrEq : Pred::HasAttr;
    } else if (c >= '0' && c <= '9') {
      p.kind = Pred::Pos;
      p.n = integer();
    } else if (is_last_call()) {
      i_ = s_.find('(', i_) + 1;
      skip_ws();
      if (i_ >= s_.size() || s_[i_] != ')') unsupported(i_);
      i_++;
      skip_ws();
      p.kind = Pred::Last;
      if (i_ < s_.size() && s_[i_] == '-') {
        i_++;
        skip_ws();
        p.n = integer();
      }
    } else if (c == '*' || c == '{' || ncname_len(i_, true)) {
      p.child = name_test();
      p.kind = eq_literal(p.lit) ? Pred::ChildEq : Pred::HasChild;
    } else {
      unsupported(i_);
    }
    skip_ws();
    if (i_ >= s_.size()) {
      fail(culebra::format("unterminated predicate at column {}", open + 1));
    }
    if (s_[i_] != ']') unsupported(i_);
    i_++;
    return p;
  }
};

template <class R>
bool name_matches(const NameTest& t, const typename R::Value& el) {
  if (R::kind(el) != VKind::Object) return false;
  if (!t.any_ns) {
    auto ns = elem_string<R>(el, "ns");
    if (ns.has_value() != t.has_ns || (ns && *ns != t.ns)) return false;
  }
  if (!t.any_local) {
    auto tag = elem_string<R>(el, "tag");
    if (!tag) return false;
    auto local = *tag;
    if (auto c = local.find(':'); c != std::string_view::npos) {
      local.remove_prefix(c + 1);
    }
    if (local != t.local) return false;
  }
  return true;
}

// A node found by a path: the chain from the context element down to it, and
// the child index at each level (document order is the indexes' order).
template <class R>
struct PathNode {
  std::vector<const typename R::Value*> chain;
  std::vector<size_t> index;
};

// `n` extended by its child `c`, the `i`th of its children.
template <class R>
PathNode<R> path_child(const PathNode<R>& n, const typename R::Value* c,
                       size_t i) {
  PathNode<R> out;
  out.chain.reserve(n.chain.size() + 1);
  out.chain.assign(n.chain.begin(), n.chain.end());
  out.chain.push_back(c);
  out.index.reserve(n.index.size() + 1);
  out.index.assign(n.index.begin(), n.index.end());
  out.index.push_back(i);
  return out;
}

template <class R>
void path_normalize(std::vector<PathNode<R>>& set) {
  using Node = PathNode<R>;
  std::sort(set.begin(), set.end(),
            [](const Node& a, const Node& b) { return a.index < b.index; });
  set.erase(std::unique(set.begin(), set.end(),
                        [](const Node& a, const Node& b) {
                          return a.index == b.index;
                        }),
            set.end());
}

template <class R>
bool pred_holds(const Pred& p, const typename R::Value& el, size_t pos,
                size_t size, std::string_view fn) {
  switch (p.kind) {
    case Pred::HasAttr: return elem_attr<R>(el, p.attr) != nullptr;
    case Pred::AttrEq: {
      const auto* v = elem_attr<R>(el, p.attr);
      if (!v) return false;
      auto text = attr_text<R>(*v);
      return text && *text == p.lit;
    }
    case Pred::HasChild:
    case Pred::ChildEq: {
      const auto* kids = elem_children<R>(el);
      if (!kids) return false;
      for (size_t i = 0; i < R::array_size(*kids); i++) {
        const auto& c = R::array_at(*kids, i);
        if (!name_matches<R>(p.child, c)) continue;
        if (p.kind == Pred::HasChild || string_value<R>(c, fn) == p.lit) {
          return true;
        }
      }
      return false;
    }
    case Pred::Pos: return static_cast<int64_t>(pos) == p.n;
    case Pred::Last: return static_cast<int64_t>(pos) == static_cast<int64_t>(size) - p.n;
  }
  return false;
}

// `//`: every node of `set` and every element below it. `set` is in document
// order without duplicates, so a node below the last one walked was walked
// with it: skipping it keeps the output in that order, without duplicates.
template <class R>
std::vector<PathNode<R>> descendant_or_self(const std::vector<PathNode<R>>& set,
                                            std::string_view fn) {
  using Node = PathNode<R>;
  std::vector<Node> all;
  const Node* walked = nullptr;
  for (const auto& n : set) {
    if (walked && n.index.size() > walked->index.size() &&
        std::equal(walked->index.begin(), walked->index.end(),
                   n.index.begin())) {
      continue;
    }
    walked = &n;
    std::vector<Node> work{n};
    while (!work.empty()) {
      Node cur = std::move(work.back());
      work.pop_back();
      if (static_cast<int64_t>(cur.index.size()) > kXmlDepthLimit) {
        throw_too_deep(fn);
      }
      if (const auto* kids = elem_children<R>(*cur.chain.back())) {
        for (size_t i = R::array_size(*kids); i-- > 0;) {
          const auto& c = R::array_at(*kids, i);
          if (R::kind(c) == VKind::Object) work.push_back(path_child<R>(cur, &c, i));
        }
      }
      all.push_back(std::move(cur));
    }
  }
  return all;
}

// A name-test step with its predicates, applied to each node of `set`.
template <class R>
std::vector<PathNode<R>> child_step(const std::vector<PathNode<R>>& set,
                                    const Step& st, std::string_view fn) {
  std::vector<PathNode<R>> next;
  std::vector<size_t> cand, kept;
  for (const auto& n : set) {
    if (static_cast<int64_t>(n.index.size()) >= kXmlDepthLimit) throw_too_deep(fn);
    const auto* kids = elem_children<R>(*n.chain.back());
    if (!kids) continue;
    cand.clear();
    for (size_t i = 0; i < R::array_size(*kids); i++) {
      if (name_matches<R>(st.test, R::array_at(*kids, i))) cand.push_back(i);
    }
    // Each predicate filters the survivors of the one before, and positions
    // count within that list (per parent, as in XPath).
    for (const auto& p : st.preds) {
      kept.clear();
      for (size_t k = 0; k < cand.size(); k++) {
        if (pred_holds<R>(p, R::array_at(*kids, cand[k]), k + 1, cand.size(),
                          fn)) {
          kept.push_back(cand[k]);
        }
      }
      cand.swap(kept);
    }
    for (size_t i : cand) next.push_back(path_child<R>(n, &R::array_at(*kids, i), i));
  }
  return next;
}

// Every match of `path` under `ctx`, in document order without duplicates.
template <class R>
std::vector<const typename R::Value*> select(const typename R::Value& ctx,
                                             std::string_view path,
                                             const NsMap& ns,
                                             std::string_view fn) {
  using V = typename R::Value;
  using Node = PathNode<R>;
  // A prefix `namespaces` does not bind falls back to the context element's
  // own `xmlns:p` declarations (first match wins, so `namespaces` does). Its
  // `xmlns=` default is not read: an unprefixed name keeps XPath's meaning.
  NsMap scope = ns;
  if (const auto* attrs = elem_attrs<R>(ctx)) {
    for (const auto& [k, v] : R::object_entries(*attrs)) {
      if (k.starts_with("xmlns:") && k.size() > 6 &&
          R::kind(*v) == VKind::String) {
        scope.emplace_back(std::string(k.substr(6)), std::string(R::as_string(*v)));
      }
    }
  }
  auto steps = PathCompiler(path, scope, fn).compile();
  std::vector<Node> set{Node{{&ctx}, {}}};
  for (const auto& st : steps) {
    if (st.descendant) set = descendant_or_self<R>(set, fn);
    // From a single node every step's result is already in document order.
    bool one = set.size() == 1;
    switch (st.kind) {
      case Step::Self:
        break;
      case Step::Parent: {
        std::vector<Node> up;
        for (auto& n : set) {
          if (n.index.empty()) continue;  // above the context element
          n.chain.pop_back();
          n.index.pop_back();
          up.push_back(std::move(n));
        }
        set = std::move(up);
        break;
      }
      case Step::Child:
        set = child_step<R>(set, st, fn);
        break;
    }
    if (!one) path_normalize<R>(set);
  }
  std::vector<const V*> out;
  out.reserve(set.size());
  for (const auto& n : set) out.push_back(n.chain.back());
  return out;
}

// --- stringify ----------------------------------------------------------------

// Copy the character at `s[i]` and return its length, refusing what no XML
// document can hold: bytes that are not UTF-8, characters outside [2] Char.
inline size_t append_checked_char(std::string& out, std::string_view s,
                                  size_t i) {
  auto b = static_cast<unsigned char>(s[i]);
  if (b >= 0x20 && b <= 0x7E) {
    out += s[i];
    return 1;
  }
  char32_t cp;
  size_t n = unicode::utf8::decode_codepoint(s.data() + i, s.size() - i, cp);
  if (n == 0) throw CulebraError("ValueError", "XML.stringify: invalid UTF-8");
  if (!is_xml_char(cp)) {
    throw CulebraError("ValueError",
                       culebra::format("XML.stringify: invalid character U+{:04X}",
                                       static_cast<uint32_t>(cp)));
  }
  out.append(s.substr(i, n));
  return n;
}
inline void escape_text(std::string& out, std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    switch (s[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\r': out += "&#13;"; break;  // a raw one would read back as '\n'
      default: i += append_checked_char(out, s, i); continue;
    }
    i++;
  }
}
inline void escape_attr(std::string& out, std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    switch (s[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '"': out += "&quot;"; break;
      case '\n': out += "&#10;"; break;
      case '\r': out += "&#13;"; break;
      case '\t': out += "&#9;"; break;
      default: i += append_checked_char(out, s, i); continue;
    }
    i++;
  }
}

template <class R>
void write_element(std::string& out, const typename R::Value& el,
                   int64_t indent, int64_t level, bool inline_only) {
  using V = typename R::Value;
  if (level >= kXmlDepthLimit) throw_too_deep("XML.stringify");
  const V* tag = R::field(el, "tag");
  if (!tag || R::kind(*tag) != VKind::String) {
    throw CulebraError("TypeError", culebra::format(
        "XML.stringify: an element's tag must be a String, got {}",
        tag ? R::type_name(*tag) : std::string_view("Nil")));
  }
  auto name = R::as_string(*tag);
  if (!is_qname(name)) {
    throw CulebraError("TypeError", culebra::format(
        "XML.stringify: invalid element name '{}'", name));
  }
  out += '<';
  out += name;
  if (const V* attrs = R::field(el, "attrs");
      attrs && R::kind(*attrs) != VKind::Nil) {
    if (R::kind(*attrs) != VKind::Object) {
      throw CulebraError("TypeError", culebra::format(
          "XML.stringify: attrs must be an Object, got {}",
          R::type_name(*attrs)));
    }
    for (const auto& [k, v] : R::object_entries(*attrs)) {
      if (!is_qname(k)) {
        throw CulebraError("TypeError", culebra::format(
            "XML.stringify: invalid attribute name '{}'", k));
      }
      auto text = attr_text<R>(*v);
      if (!text) {
        throw CulebraError("TypeError", culebra::format(
            "XML.stringify: attribute '{}' must be a String, Long, Float or "
            "Bool, got {}", k, R::type_name(*v)));
      }
      out += ' ';
      out += k;
      out += "=\"";
      escape_attr(out, *text);
      out += '"';
    }
  }
  const V* kids = R::field(el, "children");
  size_t n = 0;
  if (kids && R::kind(*kids) != VKind::Nil) {
    if (R::kind(*kids) != VKind::Array) {
      throw CulebraError("TypeError", culebra::format(
          "XML.stringify: children must be an Array, got {}",
          R::type_name(*kids)));
    }
    n = R::array_size(*kids);
  }
  if (n == 0) {
    out += "/>";
    return;
  }
  bool all_elements = true;
  for (size_t i = 0; i < n; i++) {
    auto k = R::kind(R::array_at(*kids, i));
    if (k == VKind::String) {
      all_elements = false;
    } else if (k != VKind::Object) {
      throw CulebraError("TypeError", culebra::format(
          "XML.stringify: a child must be an element Object or a String, got {}",
          R::type_name(R::array_at(*kids, i))));
    }
  }
  out += '>';
  // Indentation would add text to an element that holds text, so such an
  // element keeps its whole content on one line.
  bool pretty = indent > 0 && !inline_only && all_elements;
  for (size_t i = 0; i < n; i++) {
    const V& c = R::array_at(*kids, i);
    if (pretty) {
      out += '\n';
      out.append(static_cast<size_t>(indent * (level + 1)), ' ');
    }
    if (R::kind(c) == VKind::String) {
      escape_text(out, R::as_string(c));
    } else {
      write_element<R>(out, c, indent, level + 1, inline_only || !pretty);
    }
  }
  if (pretty) {
    out += '\n';
    out.append(static_cast<size_t>(indent * level), ' ');
  }
  out += "</";
  out += name;
  out += '>';
}

template <class R>
std::string stringify(const typename R::Value& el, int64_t indent,
                      bool declaration) {
  std::string out;
  if (declaration) out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  write_element<R>(out, el, indent, 0, false);
  return out;
}

}  // namespace culebra::xml
