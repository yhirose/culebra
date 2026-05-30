//
//  regexlib.h
//
//  A grapheme-aware, linear-time regular expression engine.
//
//  This is the PoC that is intended to grow into the standalone
//  `yhirose/cpp-regexlib` library. It currently lives inside the culebra
//  repository so it can be exercised against real use cases; once it has
//  proven itself it will move out to its own repo and be vendored back in
//  alongside cpp-httplib / cpp-unicodelib.
//
//  Two differentiators set this engine apart from PCRE2 / RE2 / Oniguruma:
//
//    1. The matching unit is the Unicode *extended grapheme cluster*, not the
//       code point. `.` consumes exactly one user-perceived character, so
//       `/.+/` against "👨‍👩‍👧‍👦" matches a single element, consistent with
//       culebra's string model (`.length` counts graphemes).
//
//    2. The engine is a Thompson NFA simulated by a Pike VM, so matching is
//       linear in the size of the subject. Catastrophic backtracking cannot
//       occur by construction. Backreferences are therefore not supported
//       (they are NP-hard and incompatible with the linear-time contract).
//
//  Match offsets are reported as byte offsets into the original UTF-8 subject
//  (culebra uses Go-style byte indexing), and always fall on grapheme
//  boundaries.
//
//  MIT License — Copyright (c) 2026 Yuji Hirose
//

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unicodelib.h>
#include <unicodelib_encodings.h>

namespace regexlib {

//===----------------------------------------------------------------------===//
// Errors
//===----------------------------------------------------------------------===//

struct RegexError : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Resource limits that bound the cost of compiling a pattern, so a small
// adversarial pattern cannot exhaust memory or the stack. A pattern that hits
// any of these raises RegexError rather than blowing up:
//   - source length    guards against giant inputs / wide alternations
//   - program size      guards against repetition blow-up (e.g. `a{1000000}`,
//                       nested `(x{1000}){1000}`)
//   - nesting depth     guards against deep recursion / stack overflow on
//                       patterns like `((((...))))`
namespace limits {
constexpr size_t kMaxPatternBytes = 1u << 15;  // 32 KiB
constexpr int kMaxProgramInsts = 1 << 18;      // 262144 compiled instructions
constexpr int kMaxNestingDepth = 1000;         // group / lookaround nesting
// Match-time budget. Matching is already linear in the subject, but the
// constant is the program size (up to kMaxProgramInsts), so a dense pattern on
// a long subject is bounded-but-slow. Cap the work per subject position; a
// match that exceeds `base + perPos * (length + 1)` raises RegexError instead
// of running for many seconds. Real patterns stay far below this.
constexpr long kStepsPerPos = 1 << 11;  // 2048 ε-closure steps per position
constexpr long kStepsBase = 1 << 20;    // slack so short subjects never trip
// Lazy-DFA state cap. A "regular" pattern can still need exponentially many DFA
// states (e.g. `.*a.{18}` needs 2^18), so bound the interned state count; the
// matcher abandons the DFA and falls back to the Pike VM (which is unaffected)
// when this is exceeded. See docs/regex_dfa_design.md.
constexpr int kMaxDfaStates = 1 << 16;  // 65536 interned DFA states
}  // namespace limits

//===----------------------------------------------------------------------===//
// Unicode character-class helpers (operate on a single code point)
//===----------------------------------------------------------------------===//

namespace detail {

inline bool is_digit_cp(char32_t cp) {
  return unicode::general_category(cp) == unicode::GeneralCategory::Nd;
}

inline bool is_word_cp(char32_t cp) {
  if (cp == U'_') return true;
  if (unicode::is_letter(cp)) return true;
  auto gc = unicode::general_category(cp);
  return gc == unicode::GeneralCategory::Nd ||
         gc == unicode::GeneralCategory::Pc;
}

inline bool is_space_cp(char32_t cp) { return unicode::is_white_space(cp); }

inline char32_t fold(char32_t cp) { return unicode::simple_case_folding(cp); }

// Unicode property classes for \p{...} / \P{...}.
enum class UProp {
  L, Lu, Ll, Lt, Lm, Lo,  // letters
  N, Nd, Nl, No,          // numbers
  P, M, S, Z, C,          // punctuation / mark / symbol / separator / other
  White                   // White_Space
};

inline bool match_uprop(UProp p, char32_t cp) {
  using GC = unicode::GeneralCategory;
  GC gc = unicode::general_category(cp);
  switch (p) {
    case UProp::L: return unicode::is_letter(cp);
    case UProp::Lu: return gc == GC::Lu;
    case UProp::Ll: return gc == GC::Ll;
    case UProp::Lt: return gc == GC::Lt;
    case UProp::Lm: return gc == GC::Lm;
    case UProp::Lo: return gc == GC::Lo;
    case UProp::N: return unicode::is_number(cp);
    case UProp::Nd: return gc == GC::Nd;
    case UProp::Nl: return gc == GC::Nl;
    case UProp::No: return gc == GC::No;
    case UProp::P: return unicode::is_punctuation(cp);
    case UProp::M: return unicode::is_mark(cp);
    case UProp::S:
      return gc == GC::Sm || gc == GC::Sc || gc == GC::Sk || gc == GC::So;
    case UProp::Z: return gc == GC::Zs || gc == GC::Zl || gc == GC::Zp;
    case UProp::C:
      return gc == GC::Cc || gc == GC::Cf || gc == GC::Cs || gc == GC::Co ||
             gc == GC::Cn;
    case UProp::White: return unicode::is_white_space(cp);
  }
  return false;
}

inline bool parse_uprop_name(const std::string &name, UProp &out) {
  static const std::unordered_map<std::string, UProp> table = {
      {"L", UProp::L},          {"Letter", UProp::L},
      {"Lu", UProp::Lu},        {"Uppercase_Letter", UProp::Lu},
      {"Ll", UProp::Ll},        {"Lowercase_Letter", UProp::Ll},
      {"Lt", UProp::Lt},        {"Lm", UProp::Lm},
      {"Lo", UProp::Lo},        {"N", UProp::N},
      {"Number", UProp::N},     {"Nd", UProp::Nd},
      {"Decimal_Number", UProp::Nd}, {"Nl", UProp::Nl},
      {"No", UProp::No},        {"P", UProp::P},
      {"Punctuation", UProp::P}, {"M", UProp::M},
      {"Mark", UProp::M},       {"S", UProp::S},
      {"Symbol", UProp::S},     {"Z", UProp::Z},
      {"Separator", UProp::Z},  {"C", UProp::C},
      {"Other", UProp::C},      {"White_Space", UProp::White},
      {"space", UProp::White},  {"Alphabetic", UProp::L},
      {"Alpha", UProp::L}};
  auto it = table.find(name);
  if (it == table.end()) return false;
  out = it->second;
  return true;
}

// A "line break" grapheme — used by `.`, `^`, `$`.
inline bool is_line_break(const std::u32string &g) {
  if (g.empty()) return false;
  char32_t c = g[0];
  return c == U'\n' || c == U'\r' || c == U'\x0b' || c == U'\x0c' ||
         c == U'\x85' || c == U' ' || c == U' ';
}

}  // namespace detail

//===----------------------------------------------------------------------===//
// Character class
//===----------------------------------------------------------------------===//

// A character class is a set of code-point ranges plus a set of named
// predefined predicates (\d \w \s and their negations). Membership is tested
// against a single grapheme: a multi-code-point cluster (e.g. an emoji ZWJ
// sequence) is never a member of a positive range/predicate, but it *does*
// satisfy a negated predicate such as \D ("not a digit").
struct CharClass {
  enum class Pred { Digit, NotDigit, Word, NotWord, Space, NotSpace };

  // A \p{...} (negate=false) or \P{...} (negate=true) member.
  struct UPropItem {
    detail::UProp prop;
    bool negate;
  };

  bool negate = false;
  std::vector<std::pair<char32_t, char32_t>> ranges;
  std::vector<Pred> preds;
  std::vector<UPropItem> uprops;

  void add_range(char32_t lo, char32_t hi) { ranges.emplace_back(lo, hi); }
  void add_pred(Pred p) { preds.push_back(p); }
  void add_uprop(detail::UProp p, bool neg) { uprops.push_back({p, neg}); }

  bool matches(const std::u32string &g, bool icase) const {
    bool single = g.size() == 1;
    char32_t cp = single ? g[0] : 0;
    char32_t fcp = (single && icase) ? detail::fold(cp) : cp;

    bool in = false;
    if (single) {
      for (auto &r : ranges) {
        if (cp >= r.first && cp <= r.second) {
          in = true;
          break;
        }
        if (icase) {
          char32_t flo = detail::fold(r.first), fhi = detail::fold(r.second);
          if (fcp >= flo && fcp <= fhi) {
            in = true;
            break;
          }
        }
      }
    }
    if (!in) {
      for (auto p : preds) {
        switch (p) {
          case Pred::Digit:
            if (single && detail::is_digit_cp(cp)) in = true;
            break;
          case Pred::NotDigit:
            if (!(single && detail::is_digit_cp(cp))) in = true;
            break;
          case Pred::Word:
            if (single && detail::is_word_cp(cp)) in = true;
            break;
          case Pred::NotWord:
            if (!(single && detail::is_word_cp(cp))) in = true;
            break;
          case Pred::Space:
            if (single && detail::is_space_cp(cp)) in = true;
            break;
          case Pred::NotSpace:
            if (!(single && detail::is_space_cp(cp))) in = true;
            break;
        }
        if (in) break;
      }
    }
    if (!in) {
      for (auto &up : uprops) {
        bool hit = single && detail::match_uprop(up.prop, cp);
        // \P{...} ("not in property") matches multi-code-point clusters too.
        if (up.negate) hit = !(single && detail::match_uprop(up.prop, cp));
        if (hit) {
          in = true;
          break;
        }
      }
    }
    return negate ? !in : in;
  }
};

//===----------------------------------------------------------------------===//
// Subject / pattern segmentation into grapheme clusters
//===----------------------------------------------------------------------===//

namespace detail {

// Holds a UTF-8 string segmented into extended grapheme clusters, retaining
// the byte offset of each cluster boundary so matches can be reported as byte
// offsets into the original string.
struct Segmented {
  std::vector<std::u32string> graphemes;  // code points of each cluster
  std::vector<size_t> byte_begin;         // size == graphemes.size() + 1
  std::string_view source;                // the original UTF-8 subject
  bool ascii = false;  // all bytes < 0x80, so grapheme index == byte index
};

inline Segmented segment(std::string_view s8) {
  // ASCII fast path: every byte < 0x80 is its own code point and its own
  // grapheme cluster, so the UTF-8 decode and grapheme-break work is skipped.
  bool ascii = true;
  for (unsigned char c : s8)
    if (c >= 0x80) {
      ascii = false;
      break;
    }
  if (ascii) {
    Segmented seg;
    size_t n = s8.size();
    seg.graphemes.resize(n);
    seg.byte_begin.resize(n + 1);
    for (size_t i = 0; i < n; i++) {
      seg.graphemes[i].assign(1, static_cast<char32_t>(
                                     static_cast<unsigned char>(s8[i])));
      seg.byte_begin[i] = i;
    }
    seg.byte_begin[n] = n;
    seg.source = s8;
    seg.ascii = true;
    return seg;
  }

  // Decode to code points while remembering each code point's byte offset.
  // Code-point count is bounded by byte count; reserve to avoid regrowth.
  std::u32string cps;
  std::vector<size_t> cp_byte;
  cps.reserve(s8.size());
  cp_byte.reserve(s8.size());
  size_t off = 0;
  while (off < s8.size()) {
    char32_t cp;
    size_t bytes;
    if (!unicode::utf8::decode_codepoint(s8.data() + off, s8.size() - off,
                                         bytes, cp)) {
      cp = static_cast<unsigned char>(s8[off]);
      bytes = 1;
    }
    cps.push_back(cp);
    cp_byte.push_back(off);
    off += bytes;
  }

  Segmented seg;
  seg.graphemes.reserve(cps.size());
  seg.byte_begin.reserve(cps.size() + 1);
  size_t i = 0;
  while (i < cps.size()) {
    size_t len = unicode::grapheme_length(cps.data() + i, cps.size() - i);
    if (len == 0) len = 1;
    seg.byte_begin.push_back(cp_byte[i]);
    seg.graphemes.emplace_back(cps.substr(i, len));
    i += len;
  }
  seg.byte_begin.push_back(s8.size());  // sentinel = end of subject
  seg.source = s8;
  return seg;
}

}  // namespace detail

//===----------------------------------------------------------------------===//
// AST
//===----------------------------------------------------------------------===//

namespace detail {

struct Node {
  enum class Kind {
    Empty,
    Lit,             // literal grapheme cluster
    AnyChar,         // .
    Class,           // [...] or \d etc.
    Concat,          // kids in sequence
    Alt,             // kids alternatives
    Repeat,          // kids[0] repeated [rmin, rmax]  (rmax = -1 → unbounded)
    Group,            // kids[0]; capturing if cap_index >= 0
    LookAround,       // kids[0]; zero-width (?=) (?!) (?<=) (?<!)
    BOL,              // ^
    EOL,              // $
    WordBoundary,     // \b
    NonWordBoundary   // \B
  };

  Kind kind = Kind::Empty;
  std::u32string lit;        // Lit
  CharClass cls;             // Class
  std::vector<Node> kids;    // Concat / Alt / Repeat / Group / LookAround
  bool greedy = true;        // Repeat
  int rmin = 0, rmax = -1;   // Repeat
  int cap_index = -1;        // Group: capture slot (>=1) or -1
  bool look_behind = false;  // LookAround
  bool look_negate = false;  // LookAround
};

// Reverse a sub-pattern so a lookbehind body can be matched right-to-left in a
// single linear pass starting at the assertion position. Leaf nodes and
// zero-width assertions are unchanged; only concatenation order flips.
// Nested lookarounds are left intact — they are evaluated forward at the
// current position regardless of the surrounding scan direction.
inline Node reverse_ast(const Node &n) {
  using K = Node::Kind;
  Node r = n;
  r.kids.clear();
  switch (n.kind) {
    case K::Concat:
      for (auto it = n.kids.rbegin(); it != n.kids.rend(); ++it)
        r.kids.push_back(reverse_ast(*it));
      break;
    case K::Alt:
    case K::Repeat:
    case K::Group:
      for (auto &k : n.kids) r.kids.push_back(reverse_ast(k));
      break;
    case K::LookAround:
      r.kids = n.kids;  // keep body as-is; compiler handles its direction
      break;
    default:
      break;  // leaf / assertion: copy as-is
  }
  return r;
}

// True if `n` can match the empty string. Used to decide whether an unbounded
// quantifier needs an empty-iteration guard (only nullable bodies do).
inline bool node_nullable(const Node &n) {
  using K = Node::Kind;
  switch (n.kind) {
    case K::Empty:
    case K::BOL:
    case K::EOL:
    case K::WordBoundary:
    case K::NonWordBoundary:
    case K::LookAround:
      return true;
    case K::Lit:
    case K::AnyChar:
    case K::Class:
      return false;
    case K::Concat:
      for (auto &k : n.kids)
        if (!node_nullable(k)) return false;
      return true;
    case K::Alt:
      for (auto &k : n.kids)
        if (node_nullable(k)) return true;
      return false;
    case K::Group:
      return node_nullable(n.kids[0]);
    case K::Repeat:
      return n.rmin == 0 || node_nullable(n.kids[0]);
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Parser  (recursive descent over a stream of pattern grapheme clusters)
//===----------------------------------------------------------------------===//

struct Parser {
  std::vector<std::u32string> toks;  // pattern segmented into graphemes
  size_t pos = 0;
  int ncap = 0;     // capture group counter (group 0 is the whole match)
  int depth = 0;    // current group/lookaround nesting depth
  bool in_look = false;  // inside a lookaround body (groups become non-capturing)
  std::unordered_map<std::string, int> &named;
  bool &icase;
  bool &multiline;
  bool &dotall;

  Parser(std::string_view pattern, std::unordered_map<std::string, int> &named_,
         bool &icase_, bool &multiline_, bool &dotall_)
      : named(named_), icase(icase_), multiline(multiline_), dotall(dotall_) {
    toks = segment(pattern).graphemes;
  }

  bool eof() const { return pos >= toks.size(); }
  const std::u32string &peek() const { return toks[pos]; }
  bool is(char32_t c) const {
    return !eof() && toks[pos].size() == 1 && toks[pos][0] == c;
  }
  bool single_cp(char32_t &out) const {
    if (eof() || toks[pos].size() != 1) return false;
    out = toks[pos][0];
    return true;
  }

  // Throw a parse error annotated with the offending position and a caret
  // line, e.g.
  //   regex parse error at position 3: nothing to repeat
  //     ab**
  //       ^
  // The caret column counts one space per preceding grapheme, so it aligns for
  // ASCII patterns (and stays close for wider clusters).
  [[noreturn]] void error(const std::string &msg) {
    std::string pat, indent;
    for (size_t i = 0; i < toks.size(); i++) {
      if (i < pos) indent += ' ';
      pat += unicode::utf8::encode(toks[i]);
    }
    throw RegexError("regex parse error at position " + std::to_string(pos) +
                     ": " + msg + "\n  " + pat + "\n  " + indent + "^");
  }

  Node parse() {
    consume_leading_flags();
    Node n = parse_alt();
    if (!eof()) error("unexpected '" + unicode::utf8::encode(peek()) + "'");
    return n;
  }

  // Recognise a leading run of inline flag groups like `(?i)` / `(?im)`.
  void consume_leading_flags() {
    while (pos + 1 < toks.size() && is(U'(') && toks[pos + 1].size() == 1 &&
           toks[pos + 1][0] == U'?') {
      size_t p = pos + 2;
      bool ti = false, tm = false, ts = false;
      while (p < toks.size() && toks[p].size() == 1 &&
             (toks[p][0] == U'i' || toks[p][0] == U'm' || toks[p][0] == U's')) {
        if (toks[p][0] == U'i') ti = true;
        if (toks[p][0] == U'm') tm = true;
        if (toks[p][0] == U's') ts = true;
        p++;
      }
      if (p < toks.size() && toks[p].size() == 1 && toks[p][0] == U')' &&
          p > pos + 2) {
        icase = icase || ti;
        multiline = multiline || tm;
        dotall = dotall || ts;
        pos = p + 1;
      } else {
        break;
      }
    }
  }

  Node parse_alt() {
    Node first = parse_concat();
    if (!is(U'|')) return first;
    Node alt;
    alt.kind = Node::Kind::Alt;
    alt.kids.push_back(std::move(first));
    while (is(U'|')) {
      pos++;
      alt.kids.push_back(parse_concat());
    }
    return alt;
  }

  Node parse_concat() {
    Node cat;
    cat.kind = Node::Kind::Concat;
    while (!eof() && !is(U'|') && !is(U')')) {
      cat.kids.push_back(parse_repeat());
    }
    if (cat.kids.size() == 1) return std::move(cat.kids[0]);
    return cat;
  }

  Node parse_repeat() {
    Node atom = parse_atom();
    if (eof()) return atom;
    char32_t c;
    if (!single_cp(c)) return atom;
    if (c == U'*' || c == U'+' || c == U'?') {
      pos++;
      Node rep;
      rep.kind = Node::Kind::Repeat;
      rep.rmin = (c == U'+') ? 1 : 0;
      rep.rmax = (c == U'?') ? 1 : -1;
      rep.greedy = !lazy_suffix();
      rep.kids.push_back(std::move(atom));
      return rep;
    }
    if (c == U'{') {
      Node rep;
      if (try_parse_counted(rep)) {
        rep.kids.push_back(std::move(atom));
        return rep;
      }
    }
    return atom;
  }

  // `?` directly after a quantifier makes it lazy.
  bool lazy_suffix() {
    if (is(U'?')) {
      pos++;
      return true;
    }
    return false;
  }

  // Parse `{n}` / `{n,}` / `{n,m}`. Returns false (without consuming) when the
  // brace does not form a valid counted quantifier, so `{` can be a literal.
  bool try_parse_counted(Node &rep) {
    size_t save = pos;
    pos++;  // consume '{'
    int lo = 0, hi = -1;
    bool have_lo = read_int(lo);
    if (!have_lo) {
      pos = save;
      return false;
    }
    if (is(U',')) {
      pos++;
      int h;
      if (read_int(h)) {
        hi = h;
      } else {
        hi = -1;  // {n,}
      }
    } else {
      hi = lo;  // {n}
    }
    if (!is(U'}')) {
      pos = save;
      return false;
    }
    pos++;  // consume '}'
    rep.kind = Node::Kind::Repeat;
    rep.rmin = lo;
    rep.rmax = hi;
    rep.greedy = !lazy_suffix();
    return true;
  }

  bool read_int(int &out) {
    if (eof() || toks[pos].size() != 1) return false;
    char32_t c = toks[pos][0];
    if (c < U'0' || c > U'9') return false;
    long v = 0;
    while (!eof() && toks[pos].size() == 1 && toks[pos][0] >= U'0' &&
           toks[pos][0] <= U'9') {
      v = v * 10 + (toks[pos][0] - U'0');
      if (v > 1000000) v = 1000000;  // clamp pathological counts
      pos++;
    }
    out = static_cast<int>(v);
    return true;
  }

  Node parse_atom() {
    char32_t c;
    if (single_cp(c)) {
      switch (c) {
        case U'(':
          return parse_group();
        case U'[':
          return parse_class();
        case U'.': {
          pos++;
          Node n;
          n.kind = Node::Kind::AnyChar;
          return n;
        }
        case U'^': {
          pos++;
          Node n;
          n.kind = Node::Kind::BOL;
          return n;
        }
        case U'$': {
          pos++;
          Node n;
          n.kind = Node::Kind::EOL;
          return n;
        }
        case U'\\':
          return parse_escape();
        case U'*':
        case U'+':
        case U'?':
          error("nothing to repeat");
        case U')':
          error("unmatched ')'");
        default:
          break;
      }
    }
    // Ordinary literal grapheme cluster.
    Node n;
    n.kind = Node::Kind::Lit;
    n.lit = peek();
    pos++;
    return n;
  }

  // RAII nesting-depth counter; throws if the pattern nests too deeply, so a
  // recursive-descent (or, later, recursive ε-closure) blow-up cannot overflow
  // the stack. Every '(' enters parse_group, so guarding it covers all nesting.
  struct DepthGuard {
    Parser &p;
    explicit DepthGuard(Parser &p_) : p(p_) {
      if (++p.depth > limits::kMaxNestingDepth) p.error("pattern nested too deeply");
    }
    ~DepthGuard() { --p.depth; }
  };

  Node parse_group() {
    DepthGuard dg(*this);
    pos++;  // consume '('
    Node g;
    g.kind = Node::Kind::Group;
    g.cap_index = -1;
    if (is(U'?')) {
      pos++;
      char32_t c;
      if (!single_cp(c)) error("malformed group");
      if (c == U'=' || c == U'!') {
        pos++;  // (?=...) / (?!...)
        return parse_lookaround(/*behind=*/false, /*negate=*/c == U'!');
      }
      // (?<= / (?<! are lookbehind; (?<name> is a named capture.
      if (c == U'<' && pos + 1 < toks.size() && toks[pos + 1].size() == 1 &&
          (toks[pos + 1][0] == U'=' || toks[pos + 1][0] == U'!')) {
        bool neg = toks[pos + 1][0] == U'!';
        pos += 2;  // consume '<' and '='/'!'
        return parse_lookaround(/*behind=*/true, neg);
      }
      if (c == U':') {
        pos++;  // non-capturing
      } else if (c == U'<' || c == U'\'') {
        // named group  (?<name>...) or (?'name'...)
        char32_t close = (c == U'<') ? U'>' : U'\'';
        pos++;
        std::u32string name;
        while (!eof() && !(toks[pos].size() == 1 && toks[pos][0] == close)) {
          name += peek();
          pos++;
        }
        if (eof()) error("unterminated group name");
        pos++;  // consume closing delimiter
        g.cap_index = assign_capture();
        if (g.cap_index >= 0) named[unicode::utf8::encode(name)] = g.cap_index;
      } else if (c == U'i' || c == U'm' || c == U's') {
        // Inline flag group encountered mid-pattern; treat as global flags.
        bool ti = false, tm = false, ts = false;
        while (!eof() && toks[pos].size() == 1 &&
               (toks[pos][0] == U'i' || toks[pos][0] == U'm' ||
                toks[pos][0] == U's')) {
          if (toks[pos][0] == U'i') ti = true;
          if (toks[pos][0] == U'm') tm = true;
          if (toks[pos][0] == U's') ts = true;
          pos++;
        }
        icase = icase || ti;
        multiline = multiline || tm;
        dotall = dotall || ts;
        if (is(U')')) {
          pos++;
          Node empty;
          empty.kind = Node::Kind::Empty;
          return empty;
        }
        if (is(U':')) {
          pos++;  // (?im:...) scoped — applied globally in this PoC
        } else {
          error("malformed inline flags");
        }
      } else {
        error("unsupported group construct '(?" + unicode::utf8::encode(peek()) +
              "'");
      }
    } else {
      g.cap_index = assign_capture();
    }
    g.kids.push_back(parse_alt());
    if (!is(U')')) error("missing ')'");
    pos++;  // consume ')'
    return g;
  }

  // Capture groups nested inside a lookaround do not export their captures in
  // this PoC, so they are compiled as non-capturing.
  int assign_capture() { return in_look ? -1 : ++ncap; }

  Node parse_lookaround(bool behind, bool negate) {
    bool save = in_look;
    in_look = true;
    Node body = parse_alt();
    in_look = save;
    if (!is(U')')) error("missing ')' in lookaround");
    pos++;  // consume ')'
    Node n;
    n.kind = Node::Kind::LookAround;
    n.look_behind = behind;
    n.look_negate = negate;
    n.kids.push_back(std::move(body));
    return n;
  }

  // Parse the body of \p / \P : either `{Name}` or a single-letter shorthand.
  detail::UProp parse_prop() {
    std::string name;
    if (is(U'{')) {
      pos++;
      while (!eof() && !is(U'}')) {
        name += unicode::utf8::encode(peek());
        pos++;
      }
      if (!is(U'}')) error("unterminated \\p{...}");
      pos++;  // consume '}'
    } else if (!eof() && toks[pos].size() == 1) {
      name = std::string(1, static_cast<char>(toks[pos][0]));
      pos++;
    } else {
      error("malformed \\p");
    }
    detail::UProp out;
    if (!detail::parse_uprop_name(name, out))
      error("unknown unicode property '" + name + "'");
    return out;
  }

  Node parse_class() {
    pos++;  // consume '['
    Node n;
    n.kind = Node::Kind::Class;
    if (is(U'^')) {
      pos++;
      n.cls.negate = true;
    }
    bool first = true;
    while (!eof() && !(is(U']') && !first)) {
      first = false;
      // Predefined class escape inside [...]
      if (is(U'\\')) {
        pos++;
        if (eof()) error("trailing backslash in class");
        char32_t e;
        if (single_cp(e) && (e == U'p' || e == U'P')) {
          bool neg = (e == U'P');
          pos++;
          n.cls.add_uprop(parse_prop(), neg);
          continue;
        }
        if (single_cp(e) && add_class_escape(n.cls, e)) {
          pos++;
          continue;
        }
        // escaped literal
        char32_t lit = escaped_literal();
        maybe_range(n.cls, lit);
        continue;
      }
      char32_t lo;
      if (!single_cp(lo)) {
        // Multi-code-point grapheme literal in a class: add each code point.
        for (char32_t cp : peek()) n.cls.add_range(cp, cp);
        pos++;
        continue;
      }
      pos++;
      maybe_range(n.cls, lo);
    }
    if (!is(U']')) error("unterminated character class");
    pos++;  // consume ']'
    return n;
  }

  // After reading a class member `lo`, check for a `lo-hi` range.
  void maybe_range(CharClass &cls, char32_t lo) {
    if (is(U'-') && pos + 1 < toks.size() &&
        !(toks[pos + 1].size() == 1 && toks[pos + 1][0] == U']')) {
      pos++;  // consume '-'
      char32_t hi;
      if (is(U'\\')) {
        pos++;
        hi = escaped_literal();
      } else if (single_cp(hi)) {
        pos++;
      } else {
        hi = lo;
      }
      if (hi < lo) std::swap(lo, hi);
      cls.add_range(lo, hi);
    } else {
      cls.add_range(lo, lo);
    }
  }

  // Returns true and records a predefined predicate; false if `e` is not one.
  static bool add_class_escape(CharClass &cls, char32_t e) {
    switch (e) {
      case U'd': cls.add_pred(CharClass::Pred::Digit); return true;
      case U'D': cls.add_pred(CharClass::Pred::NotDigit); return true;
      case U'w': cls.add_pred(CharClass::Pred::Word); return true;
      case U'W': cls.add_pred(CharClass::Pred::NotWord); return true;
      case U's': cls.add_pred(CharClass::Pred::Space); return true;
      case U'S': cls.add_pred(CharClass::Pred::NotSpace); return true;
      default: return false;
    }
  }

  // Translate an escape sequence to a literal code point (caller has already
  // consumed the backslash; current token is the escaped char).
  char32_t escaped_literal() {
    if (eof()) error("trailing backslash");  // e.g. a class range ending in `\`
    char32_t e;
    if (!single_cp(e)) {
      char32_t cp = peek()[0];
      pos++;
      return cp;
    }
    pos++;
    switch (e) {
      case U'n': return U'\n';
      case U'r': return U'\r';
      case U't': return U'\t';
      case U'f': return U'\f';
      case U'v': return U'\x0b';
      case U'0': return U'\0';
      case U'x': return parse_hex();
      case U'u': return parse_hex();
      default: return e;  // escaped metacharacter → itself
    }
  }

  // \xHH or \x{HHHH} / \uHHHH
  char32_t parse_hex() {
    auto hexval = [](char32_t c) -> int {
      if (c >= U'0' && c <= U'9') return c - U'0';
      if (c >= U'a' && c <= U'f') return c - U'a' + 10;
      if (c >= U'A' && c <= U'F') return c - U'A' + 10;
      return -1;
    };
    char32_t v = 0;
    if (is(U'{')) {
      pos++;
      while (!eof() && !is(U'}')) {
        int h = (toks[pos].size() == 1) ? hexval(toks[pos][0]) : -1;
        if (h < 0) error("invalid hex escape");
        v = v * 16 + h;
        pos++;
      }
      if (is(U'}')) pos++;
    } else {
      for (int i = 0; i < 4 && !eof(); i++) {
        if (toks[pos].size() != 1) break;
        int h = hexval(toks[pos][0]);
        if (h < 0) break;
        v = v * 16 + h;
        pos++;
      }
    }
    return v;
  }

  Node parse_escape() {
    pos++;  // consume '\'
    if (eof()) error("trailing backslash");
    char32_t e;
    if (single_cp(e)) {
      // Unicode property class \p{...} / \P{...}.
      if (e == U'p' || e == U'P') {
        bool neg = (e == U'P');
        pos++;
        detail::UProp prop = parse_prop();
        Node n;
        n.kind = Node::Kind::Class;
        n.cls.add_uprop(prop, neg);
        return n;
      }
      // Predefined classes as standalone atoms.
      CharClass cls;
      if (Parser::add_class_escape(cls, e)) {
        pos++;
        Node n;
        n.kind = Node::Kind::Class;
        n.cls = cls;
        return n;
      }
      if (e == U'b') {
        pos++;
        Node n;
        n.kind = Node::Kind::WordBoundary;
        return n;
      }
      if (e == U'B') {
        pos++;
        Node n;
        n.kind = Node::Kind::NonWordBoundary;
        return n;
      }
    }
    // Escaped literal.
    char32_t lit = escaped_literal();
    Node n;
    n.kind = Node::Kind::Lit;
    n.lit = std::u32string(1, lit);
    return n;
  }
};

//===----------------------------------------------------------------------===//
// Compiler  (AST → Pike VM program)
//===----------------------------------------------------------------------===//

struct Program;  // defined below; referenced by Inst::sub

struct Inst {
  enum class Op {
    Char,   // match literal grapheme `lit`, advance
    Class,  // match `cls`, advance
    Any,    // match any non-line-break grapheme, advance
    Split,  // fork to x (preferred) and y
    Jmp,    // jump to x
    Save,   // saves[n] = current position
    Look,   // zero-width lookaround: run `sub` at the current position
    Loop,   // empty-guarded back-edge of an unbounded quantifier over a
            // nullable body: re-enter x only if the iteration made progress
            // (sp != saves[n]); otherwise take the exit y
    AssertBOL,
    AssertEOL,
    AssertWB,
    AssertNWB,
    Match
  };

  Op op;
  std::u32string lit;  // Char
  CharClass cls;       // Class
  int x = 0, y = 0;    // Split / Jmp / Loop targets
  int n = 0;           // Save slot (also Loop progress slot)
  bool greedy = false;  // Loop: prefer re-entering over exiting
  // Look
  std::shared_ptr<Program> sub;  // compiled lookaround body
                                 // (reverse-compiled for lookbehind)
  bool look_behind = false;
  bool look_negate = false;
};

struct Program {
  std::vector<Inst> insts;
  int nslots = 2;  // 2 per group; group 0 is the whole match
};

struct Compiler {
  Program prog;
  int base_slots_ = 2;  // capture slots; hidden Loop slots are allocated above
  int loop_slots_ = 0;  // number of hidden progress slots allocated

  static Program compile(const Node &root, int ncap) {
    Compiler c;
    c.base_slots_ = 2 * (ncap + 1);
    c.emit_save(0);  // start of whole match
    c.emit(root);
    c.emit_save(1);  // end of whole match
    Inst m;
    m.op = Inst::Op::Match;
    c.prog.insts.push_back(m);
    c.prog.nslots = c.base_slots_ + c.loop_slots_;
    return std::move(c.prog);
  }

  int here() const { return static_cast<int>(prog.insts.size()); }

  int push(Inst i) {
    if (static_cast<int>(prog.insts.size()) >= limits::kMaxProgramInsts)
      throw RegexError("regex pattern too large (compiled program too big)");
    prog.insts.push_back(std::move(i));
    return here() - 1;
  }

  void emit_save(int slot) {
    Inst i;
    i.op = Inst::Op::Save;
    i.n = slot;
    push(std::move(i));
  }

  void emit(const Node &n) {
    using K = Node::Kind;
    switch (n.kind) {
      case K::Empty:
        break;
      case K::Lit: {
        Inst i;
        i.op = Inst::Op::Char;
        i.lit = n.lit;
        push(std::move(i));
        break;
      }
      case K::AnyChar: {
        Inst i;
        i.op = Inst::Op::Any;
        push(std::move(i));
        break;
      }
      case K::Class: {
        Inst i;
        i.op = Inst::Op::Class;
        i.cls = n.cls;
        push(std::move(i));
        break;
      }
      case K::BOL: {
        Inst i;
        i.op = Inst::Op::AssertBOL;
        push(std::move(i));
        break;
      }
      case K::EOL: {
        Inst i;
        i.op = Inst::Op::AssertEOL;
        push(std::move(i));
        break;
      }
      case K::WordBoundary: {
        Inst i;
        i.op = Inst::Op::AssertWB;
        push(std::move(i));
        break;
      }
      case K::NonWordBoundary: {
        Inst i;
        i.op = Inst::Op::AssertNWB;
        push(std::move(i));
        break;
      }
      case K::Concat:
        for (auto &k : n.kids) emit(k);
        break;
      case K::Alt:
        emit_alt(n.kids);
        break;
      case K::Group: {
        if (n.cap_index >= 0) emit_save(2 * n.cap_index);
        emit(n.kids[0]);
        if (n.cap_index >= 0) emit_save(2 * n.cap_index + 1);
        break;
      }
      case K::LookAround: {
        Inst i;
        i.op = Inst::Op::Look;
        i.look_behind = n.look_behind;
        i.look_negate = n.look_negate;
        // The lookaround body is its own program (captures are not exported).
        // Lookbehind bodies are reverse-compiled so they can be matched
        // right-to-left from the assertion position in one linear pass.
        i.sub = std::make_shared<Program>(
            n.look_behind ? compile(reverse_ast(n.kids[0]), 0)
                          : compile(n.kids[0], 0));
        push(std::move(i));
        break;
      }
      case K::Repeat:
        emit_repeat(n);
        break;
    }
  }

  // Left-to-right alternation, preserving priority order. Iterative so a very
  // wide alternation cannot overflow the compiler stack.
  //   split L1,Lnext ; L1: <kid0> ; jmp End ; Lnext: split ... ; <kidLast> ; End:
  void emit_alt(const std::vector<Node> &kids) {
    std::vector<int> jmps;
    for (size_t i = 0; i + 1 < kids.size(); i++) {
      int split = push_split();
      int l1 = here();
      emit(kids[i]);
      jmps.push_back(push_jmp());
      int l2 = here();  // next alternative's split (or the last kid)
      prog.insts[split].x = l1;
      prog.insts[split].y = l2;
    }
    emit(kids.back());  // last alternative: no split, fall-through target
    int end = here();
    for (int j : jmps) prog.insts[j].x = end;
  }

  int push_split() {
    Inst i;
    i.op = Inst::Op::Split;
    return push(std::move(i));
  }
  int push_jmp() {
    Inst i;
    i.op = Inst::Op::Jmp;
    return push(std::move(i));
  }

  void emit_repeat(const Node &n) {
    const Node &body = n.kids[0];
    int lo = n.rmin, hi = n.rmax;

    // Emit `lo` mandatory copies.
    for (int k = 0; k < lo; k++) emit(body);

    if (hi == -1 && node_nullable(body)) {
      // Unbounded tail over a NULLABLE body needs an empty-iteration guard, or
      // a greedy loop would keep finding a consuming path instead of stopping
      // (e.g. `(.*?)*` must match empty, not "c"). Record each iteration's
      // entry position in a hidden slot and only re-enter on real progress:
      //   L1: split L2,L3        (greedy: prefer entering)
      //   L2: save w ; <body> ; Loop(w, x=L2, y=L3)
      //   L3:
      int w = base_slots_ + loop_slots_++;
      int l1 = push_split();
      int l2 = here();
      emit_save(w);
      emit(body);
      Inst lp;
      lp.op = Inst::Op::Loop;
      lp.n = w;
      lp.x = l2;  // re-enter
      lp.greedy = n.greedy;
      int lpi = push(std::move(lp));
      int l3 = here();
      prog.insts[lpi].y = l3;  // exit
      set_split(l1, n.greedy, l2, l3);
    } else if (hi == -1) {
      // Unbounded tail over a non-nullable body: the cheap structure suffices
      // (each iteration necessarily consumes, so it cannot loop on empty).
      //   L1: split L2,L3 ; L2: body ; jmp L1 ; L3:
      int l1 = push_split();
      int l2 = here();
      emit(body);
      int j = push_jmp();
      prog.insts[j].x = l1;
      int l3 = here();
      set_split(l1, n.greedy, l2, l3);
    } else {
      // Bounded: emit (hi - lo) optional copies.
      std::vector<int> splits;
      for (int k = lo; k < hi; k++) {
        int s = push_split();
        splits.push_back(s);
        emit(body);
      }
      int end = here();
      for (int s : splits) set_split(s, n.greedy, s + 1, end);
    }
  }

  // Configure a Split honouring greedy/lazy preference.
  void set_split(int idx, bool greedy, int enter, int skip) {
    if (greedy) {
      prog.insts[idx].x = enter;
      prog.insts[idx].y = skip;
    } else {
      prog.insts[idx].x = skip;
      prog.insts[idx].y = enter;
    }
  }
};

}  // namespace detail

//===----------------------------------------------------------------------===//
// Match results
//===----------------------------------------------------------------------===//

struct Capture {
  bool matched = false;
  size_t begin = 0;  // byte offset, inclusive
  size_t end = 0;    // byte offset, exclusive
  std::string str;
};

struct MatchResult {
  bool matched = false;
  size_t begin = 0;  // byte offset of whole match start
  size_t end = 0;    // byte offset of whole match end
  std::string str;
  std::vector<Capture> groups;  // groups[0] is the whole match
  std::unordered_map<std::string, size_t> named;

  // Grapheme-index span of the whole match (used internally for stepping).
  int begin_grapheme = 0;
  int end_grapheme = 0;

  explicit operator bool() const { return matched; }

  const Capture &group(size_t i) const {
    static const Capture empty;
    return i < groups.size() ? groups[i] : empty;
  }
  const Capture &group(const std::string &name) const {
    static const Capture empty;
    auto it = named.find(name);
    return it == named.end() ? empty : group(it->second);
  }
};

//===----------------------------------------------------------------------===//
// Regex
//===----------------------------------------------------------------------===//

// Pattern-wide flags, OR-combinable: Regex("abc", IgnoreCase | Multiline).
// They are equivalent to the corresponding leading inline groups (?i)(?m)(?s);
// inline flags inside the pattern turn the same flags on in addition.
enum Flag : unsigned {
  IgnoreCase = 1u << 0,  // (?i) — case-insensitive
  Multiline = 1u << 1,   // (?m) — ^ and $ also match at line breaks
  DotAll = 1u << 2,      // (?s) — `.` also matches a line break
};

class Regex {
 public:
  explicit Regex(std::string_view pattern, unsigned flags = 0) {
    if (pattern.size() > limits::kMaxPatternBytes)
      throw RegexError("regex pattern too long");
    icase_ = (flags & IgnoreCase) != 0;
    multiline_ = (flags & Multiline) != 0;
    dotall_ = (flags & DotAll) != 0;
    detail::Parser parser(pattern, named_, icase_, multiline_, dotall_);
    detail::Node root = parser.parse();
    ncap_ = parser.ncap;
    prog_ = detail::Compiler::compile(root, ncap_);
    extract_prefix();
    analyze_dfa();
    analyze_first_bytes();
  }

  int group_count() const { return ncap_; }
  const std::unordered_map<std::string, int> &named_groups() const {
    return named_;
  }

  // Search anywhere in `text`.
  MatchResult search(std::string_view text) const {
    auto seg = detail::segment(text);
    Scratch sc;
    return search_at_or_after(seg, 0, sc);
  }

  // Match anchored at the start of `text` (need not reach the end).
  MatchResult match(std::string_view text) const {
    auto seg = detail::segment(text);
    Scratch sc;
    return run(seg, 0, /*anchored=*/true, sc);
  }

  bool test(std::string_view text) const {
    // Boolean match is the same under leftmost-first and POSIX semantics, so a
    // lazy DFA can answer it when the program is "regular" and the subject is
    // ASCII. Everything else uses the Pike VM.
    if (dfa_ok_) {
      bool ascii = true;
      for (unsigned char c : text)
        if (c >= 0x80) {
          ascii = false;
          break;
        }
      if (ascii) return dfa_test(text);
    }
    return search(text).matched;
  }

  std::vector<MatchResult> find_all(std::string_view text) const {
    auto seg = detail::segment(text);
    std::vector<MatchResult> out;
    int start = 0;
    int n = static_cast<int>(seg.graphemes.size());
    Scratch sc;  // reused across every match
    while (start <= n) {
      MatchResult m = search_at_or_after(seg, start, sc);
      if (!m.matched) break;
      out.push_back(m);
      int end_idx = m.end_grapheme;
      start = (end_idx > m.begin_grapheme) ? end_idx : end_idx + 1;
    }
    return out;
  }

  // Replace all matches. `$0`..`$9` and `$<name>` expand to captures; `$$`
  // is a literal '$'.
  std::string replace_all(std::string_view text, std::string_view repl) const {
    auto matches = find_all(text);
    std::string out;
    size_t cursor = 0;
    for (auto &m : matches) {
      out.append(text.substr(cursor, m.begin - cursor));
      out.append(expand(repl, m));
      cursor = m.end;
    }
    out.append(text.substr(cursor));
    return out;
  }

 private:
  detail::Program prog_;
  int ncap_ = 0;
  bool icase_ = false;
  bool multiline_ = false;
  bool dotall_ = false;
  std::unordered_map<std::string, int> named_;
  std::string prefix_;  // required literal prefix (empty if none); see B1 below
  std::array<bool, 128> start_bytes_{};  // bytes that can begin a match
  bool first_byte_ok_ = false;  // start_bytes_ filter is usable (see below)
  bool dfa_ok_ = false;  // program is "regular" -> boolean match via lazy DFA
  int match_pc_ = -1;    // index of the Match instruction

  // Every match must begin with the pattern's leading run of literal
  // characters. Collect it (skipping the zero-width capture Saves) so an
  // unanchored search can jump straight to the next occurrence with memmem
  // instead of seeding the NFA at every position. Skipped when case-insensitive
  // (memmem is case-sensitive).
  void extract_prefix() {
    if (icase_) return;
    using Op = detail::Inst::Op;
    for (const auto &in : prog_.insts) {
      if (in.op == Op::Save) continue;
      if (in.op == Op::Char)
        prefix_ += unicode::utf8::encode(in.lit);
      else
        break;
    }
  }

  // Generalization of the literal prefix (B1) to a leading first-byte set. When
  // the program is "regular" (no asserts/lookaround/empty-loop) and cannot match
  // empty, every match's first grapheme is one of the consuming instructions in
  // the start ε-closure. On an ASCII subject (grapheme index == byte index) we
  // can scan to the next byte that can begin a match and seed the unanchored NFA
  // there, instead of seeding it at every dead position. This covers patterns
  // with no literal prefix — `[a-z]+`, `(foo|bar)`, `\d+` — that memmem cannot.
  // The scan stays a single unanchored pass (no anchored per-candidate retry),
  // so it is linear, not quadratic.
  void analyze_first_bytes() {
    using Op = detail::Inst::Op;
    first_byte_ok_ = false;
    if (!dfa_ok_) return;  // asserts/lookaround/loop break first-byte reasoning
    std::vector<int> startpcs;
    std::vector<char> seen(prog_.insts.size(), 0);
    dfa_closure({0}, startpcs, seen);
    for (int pc : startpcs) {
      Op op = prog_.insts[pc].op;
      if (op == Op::Match) return;  // empty match possible -> every position viable
      if (op == Op::Any) return;    // matches nearly every byte -> skipping is moot
    }
    start_bytes_.fill(false);
    for (int b = 0; b < 128; b++)
      for (int pc : startpcs)
        if (inst_matches_byte(prog_.insts[pc], static_cast<unsigned char>(b))) {
          start_bytes_[b] = true;
          break;
        }
    first_byte_ok_ = true;
  }

  //===--------------------------------------------------------------------===//
  // Lazy DFA for boolean match (test) on ASCII subjects.
  //
  // Valid only for a "regular" program: no lookaround, no empty-loop guard, and
  // no positional assertions (^ $ \b) — those make a transition depend on more
  // than the current byte. A DFA state is the set of consuming/Match program
  // counters reachable by the capture-free ε-closure; states and their
  // byte-transitions are interned and built on demand. The subject is scanned
  // one byte per step, re-seeding the start so the search is unanchored.
  //===--------------------------------------------------------------------===//

  void analyze_dfa() {
    using Op = detail::Inst::Op;
    dfa_ok_ = true;
    for (size_t i = 0; i < prog_.insts.size(); i++) {
      Op op = prog_.insts[i].op;
      if (op == Op::Match) match_pc_ = static_cast<int>(i);
      if (op == Op::Look || op == Op::Loop || op == Op::AssertBOL ||
          op == Op::AssertEOL || op == Op::AssertWB || op == Op::AssertNWB)
        dfa_ok_ = false;
    }
  }

  bool inst_matches_byte(const detail::Inst &in, unsigned char b) const {
    using Op = detail::Inst::Op;
    std::u32string g(1, static_cast<char32_t>(b));
    switch (in.op) {
      case Op::Char: return lit_match(in.lit, g);
      case Op::Class: return in.cls.matches(g, icase_);
      case Op::Any: return dotall_ || !detail::is_line_break(g);
      default: return false;
    }
  }

  // Capture-free ε-closure: from `seeds`, follow Jmp/Split/Save and collect the
  // reachable consuming (Char/Class/Any) and Match program counters.
  void dfa_closure(std::vector<int> stk, std::vector<int> &out,
                   std::vector<char> &seen) const {
    using Op = detail::Inst::Op;
    while (!stk.empty()) {
      int pc = stk.back();
      stk.pop_back();
      if (seen[pc]) continue;
      seen[pc] = 1;
      const detail::Inst &in = prog_.insts[pc];
      switch (in.op) {
        case Op::Jmp: stk.push_back(in.x); break;
        case Op::Split:
          stk.push_back(in.x);
          stk.push_back(in.y);
          break;
        case Op::Save: stk.push_back(pc + 1); break;
        default: out.push_back(pc); break;  // Char/Class/Any/Match
      }
    }
  }

  bool dfa_test(std::string_view text) const {
    int psz = static_cast<int>(prog_.insts.size());
    std::map<std::vector<int>, int> intern;
    std::vector<std::vector<int>> states;
    std::vector<char> accept;
    std::vector<std::array<int, 128>> trans;
    std::vector<char> seen(psz, 0);

    auto make_state = [&](std::vector<int> pcs) -> int {
      std::sort(pcs.begin(), pcs.end());
      pcs.erase(std::unique(pcs.begin(), pcs.end()), pcs.end());
      auto it = intern.find(pcs);
      if (it != intern.end()) return it->second;
      int id = static_cast<int>(states.size());
      bool acc =
          std::find(pcs.begin(), pcs.end(), match_pc_) != pcs.end();
      intern.emplace(pcs, id);
      states.push_back(std::move(pcs));
      accept.push_back(acc);
      std::array<int, 128> t;
      t.fill(-1);
      trans.push_back(t);
      return id;
    };

    std::vector<int> startpcs;
    dfa_closure({0}, startpcs, seen);
    int start = make_state(std::move(startpcs));
    if (accept[start]) return true;

    int s = start;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(text.data());
    size_t n = text.size();
    for (size_t i = 0; i < n; i++) {
      unsigned char b = p[i];
      int nxt = trans[s][b];
      if (nxt < 0) {
        std::vector<int> seeds;
        for (int pc : states[s])
          if (pc != match_pc_ && inst_matches_byte(prog_.insts[pc], b))
            seeds.push_back(pc + 1);
        seeds.push_back(0);  // re-seed the start: unanchored search
        std::fill(seen.begin(), seen.end(), 0);
        std::vector<int> outpcs;
        dfa_closure(std::move(seeds), outpcs, seen);
        nxt = make_state(std::move(outpcs));
        trans[s][b] = nxt;  // indexed after make_state (vectors may have grown)
        // State cap: a regular pattern can still need exponentially many DFA
        // states. Abandon the DFA and let the Pike VM answer — the boolean
        // result is identical and the VM is unaffected by this blowup.
        if (static_cast<int>(states.size()) > limits::kMaxDfaStates)
          return search(text).matched;
      }
      s = nxt;
      if (accept[s]) return true;
    }
    return false;
  }


  // Capture slots are shared between threads copy-on-write: ε-transitions
  // (Jmp/Split/assert/lookaround) only bump a refcount; a Save clones the
  // vector. Matching is single-threaded, so the refcount is non-atomic — this
  // keeps the per-position scan allocation- and atomic-free.
  class Saves {
    struct Buf {
      int rc;
      std::vector<int> v;
    };
    Buf *b_ = nullptr;

    // Per-thread free-list of Bufs. Matching allocates a fresh capture buffer
    // on every Save, so pooling them avoids a malloc/free storm; a reused Buf
    // keeps its vector capacity, so the copy in write() rarely re-allocates.
    // The pool is cleaned at thread exit (no leak) and capped (no unbounded
    // growth), and is per-thread so concurrent matching stays safe.
    struct Pool {
      std::vector<Buf *> free;
      ~Pool() {
        for (Buf *b : free) delete b;
      }
    };
    static Pool &pool() {
      static thread_local Pool p;
      return p;
    }
    static Buf *take() {
      Pool &p = pool();
      if (!p.free.empty()) {
        Buf *b = p.free.back();
        p.free.pop_back();
        b->rc = 1;
        return b;
      }
      return new Buf{1, {}};
    }
    static void give(Buf *b) {
      Pool &p = pool();
      if (p.free.size() < 1024)
        p.free.push_back(b);
      else
        delete b;
    }

   public:
    Saves() = default;
    explicit Saves(int nslots) : b_(take()) { b_->v.assign(nslots, -1); }
    Saves(const Saves &o) : b_(o.b_) {
      if (b_) ++b_->rc;
    }
    Saves(Saves &&o) noexcept : b_(o.b_) { o.b_ = nullptr; }
    Saves &operator=(Saves o) noexcept {
      std::swap(b_, o.b_);
      return *this;
    }
    ~Saves() {
      if (b_ && --b_->rc == 0) give(b_);
    }
    const std::vector<int> &operator*() const { return b_->v; }
    // Clone the buffer with one slot updated (copy-on-write).
    Saves write(int slot, int sp) const {
      Saves c;
      c.b_ = take();
      c.b_->v = b_->v;  // copy; the pooled buffer's capacity is reused
      c.b_->v[slot] = sp;
      return c;
    }
  };

  struct Thread {
    int pc;
    Saves saves;
  };

  static Saves make_saves(int nslots) { return Saves(nslots); }

  // Total ε-closure work for the current top-level match (reset by run()).
  // Drives the match-time step budget. Per-thread, so concurrent matching on
  // one Regex stays safe.
  static long &match_steps() {
    static thread_local long s = 0;
    return s;
  }
  static long step_budget(int n) {
    return limits::kStepsBase + static_cast<long>(n + 1) * limits::kStepsPerPos;
  }

  // Reusable VM buffers so find_all() does not re-allocate per match. `gen` is
  // monotonic across runs, so `visited` never needs re-initialising.
  struct Scratch {
    std::vector<Thread> clist, nlist;
    std::vector<int> visited;
    int gen = 0;
    void prepare(int psz) {
      clist.clear();
      nlist.clear();
      clist.reserve(psz);
      nlist.reserve(psz);
      if (static_cast<int>(visited.size()) < psz) visited.assign(psz, -1);
    }
  };

  bool bol(const detail::Segmented &seg, int sp) const {
    if (sp == 0) return true;
    return multiline_ && detail::is_line_break(seg.graphemes[sp - 1]);
  }
  bool eol(const detail::Segmented &seg, int sp, int n) const {
    if (sp == n) return true;
    return multiline_ && detail::is_line_break(seg.graphemes[sp]);
  }
  static bool word_at(const detail::Segmented &seg, int sp, int n) {
    if (sp < 0 || sp >= n) return false;
    const auto &g = seg.graphemes[sp];
    return g.size() == 1 && detail::is_word_cp(g[0]);
  }
  bool wb(const detail::Segmented &seg, int sp, int n) const {
    return word_at(seg, sp - 1, n) != word_at(seg, sp, n);
  }

  bool lit_match(const std::u32string &pat, const std::u32string &g) const {
    if (pat.size() != g.size()) return false;
    if (!icase_) return pat == g;
    for (size_t i = 0; i < pat.size(); i++) {
      if (detail::fold(pat[i]) != detail::fold(g[i])) return false;
    }
    return true;
  }

  // Evaluate a zero-width lookaround instruction at position `sp`.
  bool eval_look(const detail::Inst &in, const detail::Segmented &seg, int sp,
                 int n) const {
    if (!in.look_behind) {
      // Lookahead: does the body match starting at sp?
      return sub_match(*in.sub, seg, sp, /*require_end=*/-1, n);
    }
    // Lookbehind: does the reverse-compiled body match some window ending at
    // sp, scanning right-to-left?
    return sub_match_reverse(*in.sub, seg, sp, n);
  }

  // Follow ε-transitions (Jmp/Split/Save/asserts/lookaround), adding reachable
  // Char/Any/Class/Match threads to `list`, de-duplicating by pc.
  void add_thread(const detail::Program &prog, std::vector<Thread> &list,
                  std::vector<int> &visited, int gen, int pc, Saves saves,
                  const detail::Segmented &seg, int sp, int n) const {
    using Op = detail::Inst::Op;
    // Iterative ε-closure with an explicit, reused stack, so a long zero-width
    // chain (e.g. `(a?){9000}`) cannot overflow the call stack. The stack is
    // thread_local (concurrent matching on one Regex stays safe) and drained
    // only down to the size on entry, so reentrant calls via lookaround share
    // it correctly. Children are pushed in reverse priority so the higher-
    // priority branch is popped first — DFS pre-order, as in the recursive form.
    static thread_local std::vector<std::pair<int, Saves>> stack;
    const size_t base = stack.size();
    stack.emplace_back(pc, std::move(saves));
    long pops = 0;
    while (stack.size() > base) {
      int p = stack.back().first;
      Saves sv = std::move(stack.back().second);
      stack.pop_back();
      ++pops;
      if (visited[p] == gen) continue;
      visited[p] = gen;
      const detail::Inst &in = prog.insts[p];
      switch (in.op) {
        case Op::Jmp:
          stack.emplace_back(in.x, std::move(sv));
          break;
        case Op::Split:
          stack.emplace_back(in.y, sv);             // lower priority (deeper)
          stack.emplace_back(in.x, std::move(sv));  // higher priority (top)
          break;
        case Op::Save:
          if (in.n >= 0 && in.n < static_cast<int>((*sv).size()))
            stack.emplace_back(p + 1, sv.write(in.n, sp));
          else
            stack.emplace_back(p + 1, std::move(sv));
          break;
        case Op::Look: {
          bool ok = eval_look(in, seg, sp, n);
          if (in.look_negate ? !ok : ok) stack.emplace_back(p + 1, std::move(sv));
          break;
        }
        case Op::Loop: {
          // Empty-iteration guard: re-enter only if the iteration progressed.
          bool progressed = in.n >= static_cast<int>((*sv).size()) ||
                            sp != (*sv)[in.n];
          if (!progressed) {
            stack.emplace_back(in.y, std::move(sv));  // empty -> exit only
          } else if (in.greedy) {
            stack.emplace_back(in.y, sv);             // exit (lower priority)
            stack.emplace_back(in.x, std::move(sv));  // re-enter (top)
          } else {
            stack.emplace_back(in.x, sv);
            stack.emplace_back(in.y, std::move(sv));
          }
          break;
        }
        case Op::AssertBOL:
          if (bol(seg, sp)) stack.emplace_back(p + 1, std::move(sv));
          break;
        case Op::AssertEOL:
          if (eol(seg, sp, n)) stack.emplace_back(p + 1, std::move(sv));
          break;
        case Op::AssertWB:
          if (wb(seg, sp, n)) stack.emplace_back(p + 1, std::move(sv));
          break;
        case Op::AssertNWB:
          if (!wb(seg, sp, n)) stack.emplace_back(p + 1, std::move(sv));
          break;
        default:
          list.push_back({p, std::move(sv)});
          break;
      }
    }
    match_steps() += pops;
  }

  // Boolean Pike-VM run for lookaround bodies: anchored at `start`, succeeds if
  // a thread reaches Match at position `require_end` (or any position when
  // require_end < 0). Captures are not tracked.
  bool sub_match(const detail::Program &prog, const detail::Segmented &seg,
                 int start, int require_end, int n) const {
    using Op = detail::Inst::Op;
    int psz = static_cast<int>(prog.insts.size());
    std::vector<Thread> clist, nlist;
    clist.reserve(psz);
    nlist.reserve(psz);
    std::vector<int> visited(psz, -1);
    int gen = 0;

    gen++;
    add_thread(prog, clist, visited, gen, 0, make_saves(prog.nslots), seg, start,
               n);

    long budget = step_budget(n);  // shared cumulative counter, not reset here
    for (int sp = start; sp <= n; sp++) {
      if (clist.empty()) break;
      if (match_steps() > budget) throw RegexError("regex match step budget exceeded");
      int next_gen = ++gen;
      const std::u32string *g = (sp < n) ? &seg.graphemes[sp] : nullptr;
      for (auto &t : clist) {
        const detail::Inst &in = prog.insts[t.pc];
        switch (in.op) {
          case Op::Char:
            if (g && lit_match(in.lit, *g))
              add_thread(prog, nlist, visited, next_gen, t.pc + 1, t.saves, seg,
                         sp + 1, n);
            break;
          case Op::Any:
            if (g && (dotall_ || !detail::is_line_break(*g)))
              add_thread(prog, nlist, visited, next_gen, t.pc + 1, t.saves, seg,
                         sp + 1, n);
            break;
          case Op::Class:
            if (g && in.cls.matches(*g, icase_))
              add_thread(prog, nlist, visited, next_gen, t.pc + 1, t.saves, seg,
                         sp + 1, n);
            break;
          case Op::Match:
            if (require_end < 0 || sp == require_end) return true;
            break;
          default:
            break;
        }
      }
      std::swap(clist, nlist);
      nlist.clear();
    }
    return false;
  }

  // Boolean Pike-VM run for lookbehind: matches a reverse-compiled body by
  // consuming graphemes leftward from `from`. Succeeds if the body matches any
  // window ending at `from`. Linear in the number of graphemes scanned.
  bool sub_match_reverse(const detail::Program &prog,
                         const detail::Segmented &seg, int from, int n) const {
    using Op = detail::Inst::Op;
    int psz = static_cast<int>(prog.insts.size());
    std::vector<Thread> clist, nlist;
    clist.reserve(psz);
    nlist.reserve(psz);
    std::vector<int> visited(psz, -1);
    int gen = 0;

    gen++;
    add_thread(prog, clist, visited, gen, 0, make_saves(prog.nslots), seg, from,
               n);

    long budget = step_budget(n);  // shared cumulative counter, not reset here
    for (int p = from; p >= 0; p--) {
      if (clist.empty()) break;
      if (match_steps() > budget) throw RegexError("regex match step budget exceeded");
      int next_gen = ++gen;
      // The grapheme immediately to the left of position p.
      const std::u32string *g = (p > 0) ? &seg.graphemes[p - 1] : nullptr;
      for (auto &t : clist) {
        const detail::Inst &in = prog.insts[t.pc];
        switch (in.op) {
          case Op::Char:
            if (g && lit_match(in.lit, *g))
              add_thread(prog, nlist, visited, next_gen, t.pc + 1, t.saves, seg,
                         p - 1, n);
            break;
          case Op::Any:
            if (g && (dotall_ || !detail::is_line_break(*g)))
              add_thread(prog, nlist, visited, next_gen, t.pc + 1, t.saves, seg,
                         p - 1, n);
            break;
          case Op::Class:
            if (g && in.cls.matches(*g, icase_))
              add_thread(prog, nlist, visited, next_gen, t.pc + 1, t.saves, seg,
                         p - 1, n);
            break;
          case Op::Match:
            return true;  // body matched some window ending at `from`
          default:
            break;
        }
      }
      std::swap(clist, nlist);
      nlist.clear();
    }
    return false;
  }

  MatchResult run(const detail::Segmented &seg, int start, bool anchored,
                  Scratch &sc) const {
    using Op = detail::Inst::Op;
    const detail::Program &prog = prog_;
    int n = static_cast<int>(seg.graphemes.size());
    int psz = static_cast<int>(prog.insts.size());

    sc.prepare(psz);
    auto &clist = sc.clist;
    auto &nlist = sc.nlist;
    auto &visited = sc.visited;
    int &gen = sc.gen;

    std::vector<int> matched;
    bool have = false;

    // The initial capture state is all-unset; since it is immutable and shared
    // copy-on-write, one instance is reused for every (re-)seed position.
    Saves seed = make_saves(prog.nslots);
    gen++;
    add_thread(prog, clist, visited, gen, 0, seed, seg, start, n);

    match_steps() = 0;  // top-level search: reset the cumulative work counter
    long budget = step_budget(n);
    for (int sp = start; sp <= n; sp++) {
      if (clist.empty() && have) break;
      if (match_steps() > budget) throw RegexError("regex match step budget exceeded");

      int next_gen = ++gen;
      const std::u32string *g = (sp < n) ? &seg.graphemes[sp] : nullptr;

      bool stop = false;
      for (size_t ti = 0; ti < clist.size() && !stop; ti++) {
        Thread &t = clist[ti];
        const detail::Inst &in = prog.insts[t.pc];
        switch (in.op) {
          case Op::Char:
            if (g && lit_match(in.lit, *g))
              add_thread(prog, nlist, visited, next_gen, t.pc + 1, t.saves, seg,
                         sp + 1, n);
            break;
          case Op::Any:
            if (g && (dotall_ || !detail::is_line_break(*g)))
              add_thread(prog, nlist, visited, next_gen, t.pc + 1, t.saves, seg,
                         sp + 1, n);
            break;
          case Op::Class:
            if (g && in.cls.matches(*g, icase_))
              add_thread(prog, nlist, visited, next_gen, t.pc + 1, t.saves, seg,
                         sp + 1, n);
            break;
          case Op::Match:
            matched = *t.saves;
            have = true;
            stop = true;  // discard lower-priority threads in this step
            break;
          default:
            break;
        }
      }

      std::swap(clist, nlist);
      nlist.clear();

      // Unanchored search: seed a fresh start thread (lowest priority) at the
      // next position, unless we already have a match.
      if (!anchored && !have && sp + 1 <= n) {
        add_thread(prog, clist, visited, next_gen, 0, seed, seg, sp + 1, n);
      }
    }

    if (!have) return MatchResult{};
    return build_result(seg, matched);
  }

  // Leftmost match at or after grapheme index `start`. With a literal prefix on
  // an ASCII subject (grapheme index == byte index), memmem skips to the first
  // prefix occurrence and the unanchored scan resumes there — any match must
  // begin with the prefix, so nothing in between can match. The scan stays
  // unanchored (one linear pass), NOT an anchored retry per occurrence: a weak
  // prefix like "a" matches at almost every position, and a per-occurrence
  // retry would be quadratic.
  MatchResult search_at_or_after(const detail::Segmented &seg, int start,
                                 Scratch &sc) const {
    if (!prefix_.empty() && seg.ascii) {
      const char *base = seg.source.data();
      size_t slen = seg.source.size();
      size_t b = static_cast<size_t>(start);
      if (b + prefix_.size() > slen) return MatchResult{};
      const void *hit =
          memmem(base + b, slen - b, prefix_.data(), prefix_.size());
      if (!hit) return MatchResult{};
      start = static_cast<int>(static_cast<const char *>(hit) - base);
    } else if (first_byte_ok_ && seg.ascii) {
      // No literal prefix, but we know the set of bytes a match can begin with:
      // skip to the next viable byte before seeding the NFA. One forward scan,
      // then one unanchored run — both linear, so find_all stays O(n).
      const char *base = seg.source.data();
      size_t slen = seg.source.size();
      size_t b = static_cast<size_t>(start);
      while (b < slen) {
        unsigned char c = static_cast<unsigned char>(base[b]);
        if (c < 128 && start_bytes_[c]) break;
        b++;
      }
      if (b >= slen) return MatchResult{};  // no byte here can begin a match
      start = static_cast<int>(b);
    }
    return run(seg, start, /*anchored=*/false, sc);
  }

  MatchResult build_result(const detail::Segmented &seg,
                           const std::vector<int> &saves) const {
    MatchResult r;
    r.matched = true;
    auto byte_of = [&](int gi) -> size_t {
      if (gi < 0) return 0;
      if (gi >= static_cast<int>(seg.byte_begin.size()))
        return seg.byte_begin.back();
      return seg.byte_begin[gi];
    };
    // Copy the matched byte range straight from the source — much cheaper than
    // re-encoding each grapheme cluster back to UTF-8.
    auto slice = [&](int gi_begin, int gi_end) -> std::string {
      size_t b = byte_of(gi_begin), e = byte_of(gi_end);
      return std::string(seg.source.substr(b, e >= b ? e - b : 0));
    };

    int whole_b = saves[0], whole_e = saves[1];
    r.begin = byte_of(whole_b);
    r.end = byte_of(whole_e);
    r.begin_grapheme = whole_b;
    r.end_grapheme = whole_e;
    r.str = slice(whole_b, whole_e);

    r.groups.resize(ncap_ + 1);
    for (int gi = 0; gi <= ncap_; gi++) {
      int b = saves[2 * gi], e = saves[2 * gi + 1];
      Capture c;
      if (b >= 0 && e >= 0 && e >= b) {
        c.matched = true;
        c.begin = byte_of(b);
        c.end = byte_of(e);
        c.str = slice(b, e);
      }
      r.groups[gi] = std::move(c);
    }
    for (auto &kv : named_) r.named[kv.first] = static_cast<size_t>(kv.second);
    return r;
  }

  std::string expand(std::string_view repl, const MatchResult &m) const {
    std::string out;
    for (size_t i = 0; i < repl.size();) {
      char ch = repl[i];
      if (ch == '$' && i + 1 < repl.size()) {
        char nx = repl[i + 1];
        if (nx == '$') {
          out += '$';
          i += 2;
          continue;
        }
        if (nx >= '0' && nx <= '9') {
          size_t idx = static_cast<size_t>(nx - '0');
          if (idx < m.groups.size()) out += m.groups[idx].str;
          i += 2;
          continue;
        }
        if (nx == '<') {
          size_t close = repl.find('>', i + 2);
          if (close != std::string_view::npos) {
            std::string name(repl.substr(i + 2, close - (i + 2)));
            out += m.group(name).str;
            i = close + 1;
            continue;
          }
        }
      }
      out += ch;
      i++;
    }
    return out;
  }
};

}  // namespace regexlib
