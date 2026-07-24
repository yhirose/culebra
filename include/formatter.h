#pragma once

// Source formatter for `culebra fmt`. Reformats Culebra source into a
// canonical style: normalized operator spacing, 2-space indentation, brace
// blocks always multi-line, and width-driven wrapping of argument lists /
// collection literals. It is a pure source -> source tool (no backend), so it
// carries none of the interp/JIT symmetry burden.
//
// Design (Phase 0):
//   * Parse to the optimized-but-not-desugared CST (parse_for_format), which
//     keeps every node's source position/length so leaf literals can be
//     re-emitted verbatim (numbers, strings, regex literals never get
//     re-encoded — only structure is normalized).
//   * Build a Wadler/Prettier-style Doc and lay it out at a target width.
//   * Any construct without an explicit printer falls back to a verbatim
//     source slice, so unknown syntax is preserved exactly rather than
//     corrupted.
//   * Safety net: the formatted output is re-parsed and its AST compared
//     structurally against the input's. A mismatch means the printer changed
//     program meaning, so we refuse to emit (never silently corrupt code).
//
// Comments are suppressed by the grammar (they never reach the AST), so
// formatting from the AST would drop them. Phase 0 therefore detects comment
// presence up front and leaves such files untouched; comment-preserving
// re-attachment is Phase 1. The comment scanner here is the reusable basis
// for that.

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "parser.h"

namespace culebra::fmt {

// ----------------------------------------------------------------------------
// Doc IR (Wadler / Prettier style)
// ----------------------------------------------------------------------------
//
// Invariant: a Hardline is never nested inside a Group. Groups decide flat-vs-
// break by width; a Hardline always breaks, which would violate that decision.
// Blocks (the only Hardline users) are emitted ungrouped, so they always lay
// out in break mode.

struct Doc;
using DocP = std::shared_ptr<Doc>;

enum class DocKind { Text, Line, SoftLine, HardLine, Group, Indent, Concat };

struct Doc {
  DocKind kind;
  std::string text;            // Text
  std::vector<DocP> children;  // Concat, Group (size 1), Indent (size 1)
  int indent = 0;              // Indent
};

inline DocP doc_text(std::string s) {
  auto d = std::make_shared<Doc>();
  d->kind = DocKind::Text;
  d->text = std::move(s);
  return d;
}
inline DocP doc_line() { auto d = std::make_shared<Doc>(); d->kind = DocKind::Line; return d; }
inline DocP doc_softline() { auto d = std::make_shared<Doc>(); d->kind = DocKind::SoftLine; return d; }
inline DocP doc_hardline() { auto d = std::make_shared<Doc>(); d->kind = DocKind::HardLine; return d; }
inline DocP doc_concat(std::vector<DocP> xs) {
  auto d = std::make_shared<Doc>();
  d->kind = DocKind::Concat;
  d->children = std::move(xs);
  return d;
}
inline DocP doc_group(DocP x) {
  auto d = std::make_shared<Doc>();
  d->kind = DocKind::Group;
  d->children = {std::move(x)};
  return d;
}
inline DocP doc_indent(int n, DocP x) {
  auto d = std::make_shared<Doc>();
  d->kind = DocKind::Indent;
  d->indent = n;
  d->children = {std::move(x)};
  return d;
}

struct LayoutCmd {
  int indent;
  bool flat;
  Doc* doc;
};

inline bool doc_fits(int remaining, std::vector<LayoutCmd> stack) {
  while (remaining >= 0) {
    if (stack.empty()) return true;
    LayoutCmd c = stack.back();
    stack.pop_back();
    switch (c.doc->kind) {
      case DocKind::Text:
        remaining -= static_cast<int>(c.doc->text.size());
        break;
      case DocKind::Concat:
        for (auto it = c.doc->children.rbegin(); it != c.doc->children.rend(); ++it)
          stack.push_back({c.indent, c.flat, it->get()});
        break;
      case DocKind::Group:
        stack.push_back({c.indent, /*flat=*/true, c.doc->children[0].get()});
        break;
      case DocKind::Indent:
        stack.push_back({c.indent + c.doc->indent, c.flat, c.doc->children[0].get()});
        break;
      case DocKind::Line:
        if (c.flat) remaining -= 1; else return true;
        break;
      case DocKind::SoftLine:
        if (!c.flat) return true;
        break;
      case DocKind::HardLine:
        return true;
    }
  }
  return false;
}

inline std::string doc_render(const DocP& root, int width) {
  std::string out;
  int col = 0;
  std::vector<LayoutCmd> stack = {{0, /*flat=*/false, root.get()}};
  while (!stack.empty()) {
    LayoutCmd c = stack.back();
    stack.pop_back();
    switch (c.doc->kind) {
      case DocKind::Text:
        out += c.doc->text;
        col += static_cast<int>(c.doc->text.size());
        break;
      case DocKind::Concat:
        for (auto it = c.doc->children.rbegin(); it != c.doc->children.rend(); ++it)
          stack.push_back({c.indent, c.flat, it->get()});
        break;
      case DocKind::Indent:
        stack.push_back({c.indent + c.doc->indent, c.flat, c.doc->children[0].get()});
        break;
      case DocKind::Group: {
        std::vector<LayoutCmd> test = stack;
        test.push_back({c.indent, /*flat=*/true, c.doc->children[0].get()});
        bool flat = doc_fits(width - col, std::move(test));
        stack.push_back({c.indent, flat, c.doc->children[0].get()});
        break;
      }
      case DocKind::Line:
        if (c.flat) { out += ' '; col += 1; }
        else { out += '\n'; out.append(c.indent, ' '); col = c.indent; }
        break;
      case DocKind::SoftLine:
        if (!c.flat) { out += '\n'; out.append(c.indent, ' '); col = c.indent; }
        break;
      case DocKind::HardLine:
        out += '\n';
        out.append(c.indent, ' ');
        col = c.indent;
        break;
    }
  }
  return out;
}

// ----------------------------------------------------------------------------
// Comment scanner (Phase 0: presence detection; Phase 1 will reuse for attach)
// ----------------------------------------------------------------------------
//
// Walks the byte stream tracking string-literal state so a `#` / `//` / `/*`
// inside a string is not mistaken for a comment, and so a comment inside a
// `"..."` / `"""..."""` interpolation `{ ... }` (an expression context) is
// still found. The five string lanes are: `'...'` (no escapes), `` `...` ``
// (raw), `"..."` and `"""..."""` (escapes + `{}` interpolation), and regex
// literals (treated as their delimiting quote — their bodies hold no comments).

struct Comment {
  size_t start;    // index of the leading `#` / `/` (first comment byte)
  size_t end;      // one past the last comment byte (line: before the EOL;
                   // block: just after `*/`). Trailing whitespace excluded.
  bool own_line;   // only whitespace precedes it on its line (else trailing)
  bool block;      // `/* ... */` vs a `#` / `//` line comment
};

struct SourceInfo {
  std::vector<Comment> comments;
  // Per-byte flag: 1 iff the byte is top-level code — not inside a string lane,
  // not inside an interpolation hole, not inside a comment. Used to brace-match
  // a block's `{ ... }` while ignoring braces inside strings / object literals'
  // own balance / interpolation holes.
  std::vector<char> is_code;
};

// Scan `s` once, producing every comment (with byte range and own-line flag)
// and the per-byte code mask. A `#` / `//` / `/*` inside a string lane
// (`'...'`, `` `...` ``, `"..."`, `"""..."""`, regex literals) is not a
// comment; a comment inside a `"..."` / `"""..."""` interpolation hole
// `{ ... }` (an expression context) is. Single source for the presence check,
// the code mask, and Phase 1 comment re-attachment.
inline SourceInfo scan_source(std::string_view s) {
  std::vector<Comment> out;
  std::vector<char> is_code(s.size(), 0);
  // Lane stack. Hole = an interpolation expression inside a DQ/Triple string;
  // it behaves like top-level code for comments and string starts, and its
  // own `{`/`}` nesting depth decides when it closes.
  enum class Ctx { Code, SQ, BT, DQ, Triple, Hole };
  struct Frame { Ctx ctx; int depth; };
  std::vector<Frame> st = {{Ctx::Code, 0}};
  size_t i = 0, n = s.size();

  auto own_line_at = [&](size_t start) {
    for (size_t j = start; j > 0;) {
      --j;
      if (s[j] == '\n') break;
      if (s[j] != ' ' && s[j] != '\t') return false;
    }
    return true;
  };
  // Record the comment beginning at `i`, advancing `i` past it.
  auto take_comment = [&] {
    is_code[i] = 0;  // the `#` / `/` was provisionally marked code; undo it
    bool own = own_line_at(i);
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
      size_t e = i + 2;
      while (e + 1 < n && !(s[e] == '*' && s[e + 1] == '/')) e++;
      e = (e + 1 < n) ? e + 2 : n;  // past the closing */
      out.push_back({i, e, own, true});
      i = e;
    } else {  // `#` or `//` line comment → to end of line
      size_t e = i;
      while (e < n && s[e] != '\n') e++;
      while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
        e--;
      out.push_back({i, e, own, false});
      i = e;  // the EOL itself is consumed by the outer loop
    }
  };
  auto is_comment_start = [&] {
    return s[i] == '#' || (s[i] == '/' && i + 1 < n &&
                           (s[i + 1] == '/' || s[i + 1] == '*'));
  };

  while (i < n) {
    Ctx c = st.back().ctx;
    char ch = s[i];
    if (c == Ctx::Code) is_code[i] = 1;  // top-level code byte (not in a hole)
    if (c == Ctx::Code || c == Ctx::Hole) {
      // Detect comments only in real top-level code, NOT inside a string
      // interpolation hole: a `#` there is far more likely a format spec
      // (`"{n:#x}"`) than a comment, and comments-in-interpolation don't occur
      // in practice. The hole is still scanned for brace balance / nested
      // strings so it terminates correctly.
      if (c == Ctx::Code && is_comment_start()) { take_comment(); continue; }
      if (ch == '\'') { st.push_back({Ctx::SQ, 0}); i++; continue; }
      if (ch == '`') { st.push_back({Ctx::BT, 0}); i++; continue; }
      if (ch == '"') {
        if (i + 2 < n && s[i + 1] == '"' && s[i + 2] == '"') {
          st.push_back({Ctx::Triple, 0}); i += 3; continue;
        }
        st.push_back({Ctx::DQ, 0}); i++; continue;
      }
      if (c == Ctx::Hole) {
        if (ch == '{') st.back().depth++;
        else if (ch == '}') {
          if (st.back().depth > 0) st.back().depth--;
          else st.pop_back();  // close the interpolation hole
        }
      }
      i++;
      continue;
    }
    if (c == Ctx::SQ) { if (ch == '\'') st.pop_back(); i++; continue; }
    if (c == Ctx::BT) { if (ch == '`') st.pop_back(); i++; continue; }
    // DQ / Triple
    if (ch == '\\') { i += 2; continue; }
    if (ch == '{') { st.push_back({Ctx::Hole, 0}); i++; continue; }
    if (c == Ctx::DQ && ch == '"') { st.pop_back(); i++; continue; }
    if (c == Ctx::Triple && ch == '"' && i + 2 < n && s[i + 1] == '"' &&
        s[i + 2] == '"') {
      st.pop_back(); i += 3; continue;
    }
    i++;
  }
  return {std::move(out), std::move(is_code)};
}

inline std::vector<Comment> scan_comments(std::string_view s) {
  return scan_source(s).comments;
}
inline bool scan_has_comment(std::string_view s) {
  return !scan_comments(s).empty();
}

// ----------------------------------------------------------------------------
// AST structural equality (safety net)
// ----------------------------------------------------------------------------

// A multi-target `for k, v` binding parses to a FOR_BINDING node; the
// formatter renders it parenthesized (`for (k, v)`), which re-parses to a
// TUPLE_PATTERN. The two names denote the same loop-target tuple and only
// ever interchange in this for-binding context, so the safety net treats
// them as equal rather than flagging a spurious meaning change.
inline std::string_view normalized_pattern_name(std::string_view name) {
  return name == "FOR_BINDING" ? std::string_view("TUPLE_PATTERN") : name;
}

inline bool ast_equal(const peg::Ast& a, const peg::Ast& b) {
  if (normalized_pattern_name(a.name) != normalized_pattern_name(b.name))
    return false;
  if (a.is_token != b.is_token) return false;
  if (a.is_token) return a.token == b.token;
  if (a.nodes.size() != b.nodes.size()) return false;
  for (size_t i = 0; i < a.nodes.size(); i++)
    if (!ast_equal(*a.nodes[i], *b.nodes[i])) return false;
  return true;
}

// ----------------------------------------------------------------------------
// Printer
// ----------------------------------------------------------------------------

class Printer {
 public:
  explicit Printer(std::string_view src) : src_(src) {
    auto info = scan_source(src);
    comments_ = std::move(info.comments);
    is_code_ = std::move(info.is_code);
  }

  DocP print_program(const peg::Ast& program) {
    // The whole file is the top-level statement list; its comment range is the
    // entire source.
    return print_statement_list(stmt_children(program), 0, src_.size());
  }

 private:
  std::string_view src_;
  std::vector<Comment> comments_;  // sorted by start (scan order)
  std::vector<char> is_code_;      // per-byte code mask (see scan_source)
  static constexpr int kIndent = 2;

  std::string slice(const peg::Ast& a) const {
    return std::string(src_.substr(a.position, a.length));
  }
  DocP verbatim(const peg::Ast& a) const { return doc_text(slice(a)); }

  // A leaf atom (IDENTIFIER / NUMBER / STRING / ...) carries an exact token,
  // but the AstOptimizer may have collapsed a delimiter-adding wrapper onto it
  // (a single-statement block `{ x }` or a parenthesized atom `( x )`), leaving
  // the node's source span widened to include those brackets while its `.token`
  // drops any string quotes. Recover the tight literal by stripping balanced
  // wrapping brackets and surrounding whitespace from the span — preserving
  // string quotes and shedding redundant parens / braces.
  // Strip balanced wrapper brackets (`{}` block, `()` paren, `[]` index) and
  // surrounding whitespace that the AstOptimizer folded into a node's span when
  // it collapsed a single-child delimiter rule onto an inner expression. The
  // node's intrinsic literal (a string's quotes, a keyword construct) is never
  // a bracket pair at the outer edge, and every intrinsic bracket/brace literal
  // (array / object / tuple / set) is printed explicitly and never reaches
  // here, so trimming is meaning-preserving.
  // True iff the leading bracket of `t` is closed by the bracket at its very
  // end (so the pair encloses the whole span) rather than by some interior
  // bracket — `(a) = (b)` must NOT be treated as a wrapped `a) = (b`. The scan
  // is string-naive, which is sufficient because it is only consulted for
  // wrapper-collapsed nodes (a single inner expression), and any residual
  // mistake is caught by the re-parse safety check.
  static bool encloses_whole(std::string_view t, char open, char close) {
    int depth = 0;
    for (size_t i = 0; i < t.size(); i++) {
      if (t[i] == open) depth++;
      else if (t[i] == close) { if (--depth == 0) return i == t.size() - 1; }
    }
    return false;
  }

  std::string_view tight_span(const peg::Ast& a) const {
    std::string_view t = src_.substr(a.position, a.length);
    for (;;) {
      while (!t.empty() && (t.front() == ' ' || t.front() == '\t' ||
                            t.front() == '\n' || t.front() == '\r'))
        t.remove_prefix(1);
      while (!t.empty() && (t.back() == ' ' || t.back() == '\t' ||
                            t.back() == '\n' || t.back() == '\r'))
        t.remove_suffix(1);
      // `?[ expr ]` (a collapsed SAFE_INDEX) wraps with a two-char prefix.
      if (t.size() >= 3 && t[0] == '?' && t[1] == '[' && t.back() == ']' &&
          encloses_whole(t.substr(1), '[', ']')) {
        t.remove_prefix(2);
        t.remove_suffix(1);
        continue;
      }
      if (t.size() >= 2 && ((t.front() == '{' && encloses_whole(t, '{', '}')) ||
                            (t.front() == '(' && encloses_whole(t, '(', ')')) ||
                            (t.front() == '[' && encloses_whole(t, '[', ']')))) {
        t.remove_prefix(1);
        t.remove_suffix(1);
        continue;
      }
      break;
    }
    return t;
  }

  // A node whose source span the optimizer widened by folding a single-child
  // delimiter rule (block / index / paren) onto an inner expression: its
  // `original_name` is that wrapper rule while its `name` is the inner tag.
  static bool is_wrapper_collapsed(const peg::Ast& a) {
    if (a.original_name == a.name) return false;
    return a.original_name == "BLOCK" || a.original_name == "LEXICAL_SCOPE" ||
           a.original_name == "INDEX" || a.original_name == "SAFE_INDEX" ||
           a.original_name == "PRIMARY";
  }
  DocP leaf(const peg::Ast& a) const { return doc_text(std::string(tight_span(a))); }

  // Precedence of an expression node by its (optimized) name. Atoms / leaves
  // and anything unlisted bind tightest (16) so they never get parenthesized.
  static int prec(const std::string& name) {
    if (name == "CONDITIONAL") return 1;
    if (name == "NIL_COALESCE") return 2;
    if (name == "LOGICAL_OR") return 3;
    if (name == "LOGICAL_AND") return 4;
    if (name == "CONDITION") return 5;
    if (name == "BIT_OR") return 6;
    if (name == "BIT_XOR") return 7;
    if (name == "BIT_AND") return 8;
    if (name == "SHIFT") return 9;
    if (name == "RANGE") return 10;
    if (name == "ADDITIVE") return 11;
    if (name == "MULTIPLICATIVE") return 12;
    if (name == "UNARY_PLUS" || name == "UNARY_MINUS" || name == "UNARY_NOT" ||
        name == "UNARY_BNOT")
      return 13;
    if (name == "POWER") return 14;
    if (name == "CALL") return 15;
    return 16;
  }

  static bool is_binary_explicit(const std::string& n) {
    return n == "ADDITIVE" || n == "MULTIPLICATIVE" || n == "CONDITION" ||
           n == "BIT_OR" || n == "BIT_XOR" || n == "BIT_AND" || n == "SHIFT" ||
           n == "POWER";
  }
  static bool is_binary_implicit(const std::string& n) {
    return n == "LOGICAL_OR" || n == "LOGICAL_AND" || n == "NIL_COALESCE";
  }
  static bool is_unary(const std::string& n) {
    return n == "UNARY_PLUS" || n == "UNARY_MINUS" || n == "UNARY_NOT" ||
           n == "UNARY_BNOT";
  }

  std::vector<const peg::Ast*> stmt_children(const peg::Ast& node) {
    // A block / program that holds 2+ statements is a STATEMENTS node; a
    // single statement collapses to itself. Distinguish by name.
    std::vector<const peg::Ast*> out;
    if (node.name == "STATEMENTS") {
      for (auto& c : node.nodes) out.push_back(c.get());
    } else {
      out.push_back(&node);
    }
    return out;
  }

  // True if the source between byte `end` and `start` holds a blank line, so we
  // preserve a single intentional blank (collapsing runs of >=2 to one).
  bool blank_gap(size_t end, size_t start) const {
    if (start <= end || start > src_.size()) return false;
    int newlines = 0;
    for (size_t i = end; i < start; i++)
      if (src_[i] == '\n') newlines++;
    return newlines >= 2;
  }
  static size_t node_end(const peg::Ast& a) { return a.position + a.length; }

  // The tight source extent of a node: the min/max byte offsets of its
  // descendant tokens (string_views into the source, whose offsets the
  // optimizer never widens — unlike position/length, which a single-child
  // collapse inflates to the enclosing rule's span). Falls back to the node's
  // own span if it has no tokens (e.g. an empty block).
  std::pair<size_t, size_t> real_span(const peg::Ast& n) const {
    size_t lo = SIZE_MAX, hi = 0;
    const char* base = src_.data();
    auto visit = [&](auto&& self, const peg::Ast& a) -> void {
      if (a.is_token && !a.token.empty() && a.token.data() >= base &&
          a.token.data() + a.token.size() <= base + src_.size()) {
        size_t s = a.token.data() - base;
        lo = std::min(lo, s);
        hi = std::max(hi, s + a.token.size());
      }
      for (const auto& c : a.nodes) self(self, *c);
    };
    visit(visit, n);
    if (lo == SIZE_MAX) return {n.position, node_end(n)};
    return {lo, hi};
  }

  // The real source extent of a statement for comment attachment: the code
  // tight range — first to last non-whitespace code byte of its (possibly
  // optimizer-widened) span — so a leading comment swallowed into the span
  // isn't treated as part of the statement.
  std::pair<size_t, size_t> stmt_extent(const peg::Ast& s) const {
    size_t ts = s.position, te = node_end(s);
    auto skippable = [&](size_t b) {
      char ch = src_[b];
      return !is_code_[b] || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    };
    while (ts < te && skippable(ts)) ts++;
    while (te > ts && skippable(te - 1)) te--;
    return {ts, te};
  }

  // The interior byte range of the first brace block at or after `from`:
  // returns {lo = just after `{`, hi = the matching `}`, after = hi + 1}. The
  // scan ignores braces inside strings / interpolation holes via is_code_, and
  // balances object / set / nested-block braces. `found` is false if no block
  // brace exists (shouldn't happen for a real block).
  struct BlockSpan { size_t lo, hi, after; bool found; };
  BlockSpan block_interior(size_t from) const {
    size_t i = from, n = src_.size();
    while (i < n && !(is_code_[i] && src_[i] == '{')) i++;
    if (i >= n) return {from, from, from, false};
    size_t lo = i + 1;
    int depth = 0;
    for (; i < n; i++) {
      if (!is_code_[i]) continue;
      if (src_[i] == '{') depth++;
      else if (src_[i] == '}' && --depth == 0)
        return {lo, i, i + 1, true};
    }
    return {lo, n, n, false};
  }

  // Comment text as a single Doc line, trailing whitespace already trimmed by
  // the scanner. (Multi-line block comments keep their embedded newlines.)
  DocP comment_doc(const Comment& c) const {
    return doc_text(std::string(src_.substr(c.start, c.end - c.start)));
  }

  // Render a statement list occupying source range [lo, hi). Comments in the
  // gaps between this level's statements are attached: an own-line comment
  // becomes a standalone line before the following statement; a same-line
  // comment trails the preceding statement. Comments inside a statement's own
  // span are left to that statement's printer (nested blocks recurse with their
  // own range); any it doesn't emit is caught by the comment-preservation check.
  DocP print_statement_list(const std::vector<const peg::Ast*>& stmts, size_t lo,
                            size_t hi) {
    return print_items(stmts, lo, hi, "",
                       [&](size_t k) { return print(*stmts[k]); });
  }

  // Render a comma/blank-separated list of member nodes occupying source range
  // [lo, hi), with comments attached. Shared by statement blocks, match/cond
  // arms, class members, and enum variants. `render(k)` produces the body of
  // item k (without the trailing `sep`); `sep` is "" (statements/members) or
  // "," (arms/variants), appended after each item before any trailing comment.
  //
  // Comment attachment hinges on three positions per item, all of which the AST
  // optimizer's single-child span widening can corrupt (a lone item inherits
  // its block's span, swallowing the `{ }` and a leading comment):
  //   * extent — code-tight span (keyword..closing delimiter), for ordering and
  //     blank-line math;
  //   * head/tail — first/last descendant-token offset (never widened), to test
  //     whether a comment lies between an item's tokens;
  //   * first-token position — extent start past a swallowed `{` / comment, the
  //     sort key, so an item never sorts before its own leading comment.
  template <typename Render>
  DocP print_items(const std::vector<const peg::Ast*>& items, size_t lo,
                   size_t hi, const std::string& sep, Render render) {
    struct Entry { size_t sort, gstart, gend; DocP doc; };
    std::vector<Entry> entries;

    std::vector<std::pair<size_t, size_t>> extent(items.size());
    std::vector<size_t> head(items.size()), tail(items.size());
    for (size_t k = 0; k < items.size(); k++) {
      extent[k] = stmt_extent(*items[k]);
      auto [rlo, rhi] = real_span(*items[k]);
      head[k] = rlo;
      tail[k] = rhi;
    }

    auto brace_depth = [&](size_t from, size_t p) {
      int d = 0;
      for (size_t i = from; i < p && i < src_.size(); i++) {
        if (!is_code_[i]) continue;
        if (src_[i] == '{') d++;
        else if (src_[i] == '}') d--;
      }
      return d;
    };
    // "Inside an item" = enclosed by a `{` the item opened (handled by its
    // recursion / verbatim slice) OR strictly between some item's first and
    // last token (e.g. a comment on a wrapped chain's continuation line).
    auto inside_item = [&](size_t p) {
      if (brace_depth(lo, p) > 0) return true;
      for (size_t k = 0; k < items.size(); k++)
        if (head[k] <= p && p < tail[k]) return true;
      return false;
    };
    // A mid-item comment (between first and last token, brace depth 0) is one
    // the item's printer can't place; the item is emitted verbatim to keep it.
    // A bare block (LEXICAL_SCOPE) is exempt: unlike keyword-led constructs its
    // first token sits *inside* its own `{`, so a comment among its statements
    // would read as brace-depth 0 — yet its printer (print_lexical_scope) places
    // every interior comment, so verbatim would only corrupt it (double braces).
    auto has_mid_comment = [&](size_t k) {
      if (items[k]->name == "LEXICAL_SCOPE") return false;
      for (const auto& c : comments_)
        if (head[k] < c.start && c.start < tail[k] &&
            brace_depth(head[k], c.start) == 0)
          return true;
      return false;
    };
    auto prev_item_index = [&](size_t p) -> int {
      int best = -1;
      for (int k = 0; k < (int)items.size(); k++)
        if (tail[k] <= p) best = k;
      return best;
    };
    auto first_token_pos = [&](size_t k) {
      size_t p = std::max(extent[k].first, lo);
      while (p < extent[k].second &&
             (!is_code_[p] || src_[p] == ' ' || src_[p] == '\t' ||
              src_[p] == '\n' || src_[p] == '\r' || src_[p] == '{' ||
              src_[p] == '(' || src_[p] == '['))
        p++;
      return p;
    };

    std::vector<DocP> trailing(items.size());
    for (size_t k = 0; k < items.size(); k++) {
      DocP doc = has_mid_comment(k)
                     ? doc_text(std::string(src_.substr(
                           extent[k].first, extent[k].second - extent[k].first)))
                     : render(k);
      if (!sep.empty()) doc = doc_concat({doc, doc_text(sep)});
      entries.push_back({first_token_pos(k), extent[k].first, extent[k].second, doc});
    }

    for (const auto& c : comments_) {
      if (c.start < lo || c.start >= hi) continue;
      if (inside_item(c.start)) continue;  // handled by the item's printer
      if (!c.own_line) {
        int pk = prev_item_index(c.start);
        if (pk >= 0) {
          trailing[pk] = trailing[pk]
              ? doc_concat({trailing[pk], doc_text("  "), comment_doc(c)})
              : doc_concat({doc_text("  "), comment_doc(c)});
          continue;
        }
      }
      entries.push_back({c.start, c.start, c.end, comment_doc(c)});  // standalone
    }

    for (size_t k = 0; k < items.size(); k++)
      if (trailing[k]) entries[k].doc = doc_concat({entries[k].doc, trailing[k]});

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.sort < b.sort; });

    std::vector<DocP> parts;
    for (size_t k = 0; k < entries.size(); k++) {
      if (k > 0) {
        parts.push_back(doc_hardline());
        if (blank_gap(entries[k - 1].gend, entries[k].gstart)) parts.push_back(doc_hardline());
      }
      parts.push_back(entries[k].doc);
    }
    return doc_concat(std::move(parts));
  }

  // A `{ ... }` brace group, always multi-line, holding `items` rendered by
  // `render` (with comments attached) and separated by `sep` ("" or ","). The
  // brace interior is located by scanning from `after_pos` (at or before `{`).
  template <typename Render>
  DocP print_braced(const std::vector<const peg::Ast*>& items, size_t after_pos,
                    const std::string& sep, bool empty_ok, Render render) {
    BlockSpan span = block_interior(after_pos);
    size_t lo = span.found ? span.lo : (items.empty() ? after_pos : items.front()->position);
    size_t hi = span.found ? span.hi : (items.empty() ? after_pos : node_end(*items.back()));

    if (items.empty()) {  // `{}` unless it holds dangling comments
      std::vector<DocP> dangling;
      for (const auto& c : comments_)
        if (c.start >= lo && c.start < hi) dangling.push_back(comment_doc(c));
      if (dangling.empty()) return empty_ok ? doc_text("{}") : doc_text("{\n}");
      std::vector<DocP> inner = {doc_hardline()};
      for (size_t k = 0; k < dangling.size(); k++) {
        if (k) inner.push_back(doc_hardline());
        inner.push_back(dangling[k]);
      }
      return doc_concat({doc_text("{"), doc_indent(kIndent, doc_concat(std::move(inner))),
                         doc_hardline(), doc_text("}")});
    }
    return doc_concat({
        doc_text("{"),
        doc_indent(kIndent, doc_concat({doc_hardline(),
                                        print_items(items, lo, hi, sep, render)})),
        doc_hardline(),
        doc_text("}"),
    });
  }

  // A brace block of statements: `body` is the block's statement node(s),
  // `after_pos` at or before its `{`.
  DocP print_block(const peg::Ast& body, size_t after_pos, bool empty_ok) {
    auto stmts = stmt_children(body);
    return print_braced(stmts, after_pos, "", empty_ok,
                        [&](size_t k) { return print(*stmts[k]); });
  }

  // A bare `{ ... }` statement block. The optimizer keeps LEXICAL_SCOPE as a
  // distinct node (a real variable scope), so it must always render braced —
  // never shed its braces via the wrapper-collapse fallback. Its lone child is
  // the STATEMENTS list (or a single collapsed statement); print that as a
  // braced block. When a function/loop body is itself a single bare block, the
  // enclosing BLOCK folds onto this LEXICAL_SCOPE (original_name=="BLOCK") — the
  // fold is why the wrapper-collapse path used to strip the inner braces.
  DocP print_lexical_scope(const peg::Ast& node) {
    if (node.nodes.empty()) return doc_text("{}");
    // node.position may point at an *enclosing* `{` that the optimizer folded
    // onto this scope (a fn/loop body that is a single bare block), so scanning
    // from it would locate the wrong brace and mis-attribute interior comments.
    // The lone child's span is folded to start exactly at this scope's own `{`.
    const peg::Ast& body = *node.nodes[0];
    return print_block(body, body.position, /*empty_ok=*/true);
  }

  // Print `node` as an operand of a binary/unary context, parenthesizing when
  // its own precedence would otherwise re-associate against `parent_prec`.
  DocP print_operand(const peg::Ast& node, int parent_prec, bool assoc_safe) {
    int cp = prec(node.name);
    bool paren = cp < parent_prec || (cp == parent_prec && !assoc_safe);
    DocP d = print(node);
    if (paren) return doc_concat({doc_text("("), d, doc_text(")")});
    return d;
  }

  DocP print_binary(const peg::Ast& node) {
    int P = prec(node.name);
    bool right_assoc = (node.name == "POWER");

    std::vector<const peg::Ast*> operands;
    std::vector<std::string> ops;
    if (is_binary_implicit(node.name)) {
      std::string op = node.name == "LOGICAL_OR" ? "||"
                     : node.name == "LOGICAL_AND" ? "&&"
                                                  : "??";
      for (auto& c : node.nodes) operands.push_back(c.get());
      for (size_t k = 1; k < operands.size(); k++) ops.push_back(op);
    } else {
      for (size_t i = 0; i < node.nodes.size(); i++) {
        if (i % 2 == 0) operands.push_back(node.nodes[i].get());
        else ops.push_back(std::string(node.nodes[i]->token));
      }
    }

    size_t m = operands.size();
    // Flat: `a op b op c`. When too wide, break AFTER each operator so it ends
    // the line and the continuation is indented:
    //   a op
    //     b op
    //     c
    // Breaking *before* the operator would change parsing: the `+`-family
    // operators sit on a no-newline (`_h_`) rung, so a newline preceding one
    // ends the expression. The post-operator `_` always permits the newline,
    // so trailing the operator is safe for every binary operator.
    std::vector<DocP> cont;
    for (size_t k = 1; k < m; k++) {
      bool assoc_safe = right_assoc ? (k == m - 1) : false;
      cont.push_back(doc_text(" " + ops[k - 1]));
      cont.push_back(doc_line());
      cont.push_back(print_operand(*operands[k], P, assoc_safe));
    }
    return doc_group(doc_concat({
        print_operand(*operands[0], P, /*assoc_safe=*/!right_assoc),
        doc_indent(kIndent, doc_concat(std::move(cont))),
    }));
  }

  DocP print_range(const peg::Ast& node) {
    // `a..b`, `a..=b`, `a..`, `..b`, `..` — no spaces around the operator.
    std::vector<DocP> parts;
    for (auto& c : node.nodes) {
      if (c->name == "RANGE_OPERATOR") parts.push_back(doc_text(std::string(c->token)));
      else parts.push_back(print_operand(*c, prec("RANGE"), /*assoc_safe=*/true));
    }
    return doc_concat(std::move(parts));
  }

  DocP print_unary(const peg::Ast& node) {
    // [OPERATOR_token, operand]
    std::string op = std::string(node.nodes[0]->token);
    // Operand needs parens if it binds looser than unary (prec < 14), so a
    // nested unary or any binary is wrapped: `-(a + b)`, `-(-x)`.
    DocP operand = print_operand(*node.nodes[1], /*parent_prec=*/14, /*assoc_safe=*/true);
    return doc_concat({doc_text(op), operand});
  }

  DocP print_ternary(const peg::Ast& node) {
    // [cond, then, else]
    DocP cond = print_operand(*node.nodes[0], prec("CONDITIONAL"), /*assoc_safe=*/true);
    DocP then = print(*node.nodes[1]);
    DocP els = print_operand(*node.nodes[2], prec("CONDITIONAL"), /*assoc_safe=*/true);
    return doc_concat({cond, doc_text(" ? "), then, doc_text(" : "), els});
  }

  // A comma-separated, group-wrappable list: `( a, b, c )` style. `open`/`close`
  // are the delimiters; each element is already a Doc.
  DocP print_delimited(const std::string& open, std::vector<DocP> items,
                       const std::string& close) {
    if (items.empty()) return doc_text(open + close);
    std::vector<DocP> inner;
    inner.push_back(doc_softline());
    for (size_t i = 0; i < items.size(); i++) {
      inner.push_back(items[i]);
      if (i + 1 < items.size()) {
        inner.push_back(doc_text(","));
        inner.push_back(doc_line());
      }
    }
    return doc_group(doc_concat({
        doc_text(open),
        doc_indent(kIndent, doc_concat(std::move(inner))),
        doc_softline(),
        doc_text(close),
    }));
  }

  // Postfix chain segments after the receiver (DOT / INDEX / ARGUMENTS / ...).
  DocP print_postfix(const peg::Ast& seg) {
    const std::string& on = seg.original_name;
    if (on == "DOT") return doc_concat({doc_text("."), doc_text(std::string(seg.token))});
    if (on == "SAFE_DOT") return doc_concat({doc_text("?."), doc_text(std::string(seg.token))});
    if (on == "NONNULL") return doc_text("!!");
    // INDEX / SAFE_INDEX are `'[' EXPRESSION ']'`: the single child means the
    // optimizer collapses the bracket node onto the index expression, so the
    // segment *is* that expression (with `[ ]` folded into its span). Print the
    // segment as the inner expression and re-add the brackets.
    if (on == "INDEX") return doc_concat({doc_text("["), print(seg), doc_text("]")});
    if (on == "SAFE_INDEX") return doc_concat({doc_text("?["), print(seg), doc_text("]")});
    if (on == "ARGUMENTS") {
      std::vector<DocP> args;
      for (auto& a : seg.nodes) args.push_back(print_arg(*a));
      return print_delimited("(", std::move(args), ")");
    }
    return verbatim(seg);
  }

  DocP print_arg(const peg::Ast& arg) {
    if (arg.original_name == "KWARG")
      return doc_concat({doc_text(std::string(arg.nodes[0]->token)), doc_text(": "),
                         print(*arg.nodes[1])});
    if (arg.original_name == "KWARG_SPLAT")
      return doc_concat({doc_text("**"), print(*arg.nodes[0])});
    return print(arg);
  }

  DocP print_call(const peg::Ast& node) {
    // The receiver must keep parens when it binds looser than a postfix op, so
    // `(-3).double()` / `(a + b).x` don't collapse to `-3.double()` (which
    // parses as `-(3.double())`) or `a + b.x`.
    DocP receiver = print_operand(*node.nodes[0], /*parent_prec=*/prec("CALL"),
                                  /*assoc_safe=*/true);
    auto is_dot = [](const peg::Ast& s) {
      return s.original_name == "DOT" || s.original_name == "SAFE_DOT";
    };
    // Count method-call segments (`.name(...)`). A chain of two or more is
    // wrappable: when too wide it breaks before each `.` (DOT sits on a rung
    // that permits a preceding newline), keeping each call on its own line
    // instead of wrapping one call's arguments mid-chain.
    int calls = 0;
    for (size_t i = 1; i + 1 < node.nodes.size(); i++)
      if (is_dot(*node.nodes[i]) && node.nodes[i + 1]->original_name == "ARGUMENTS")
        calls++;

    std::vector<DocP> parts;
    if (calls < 2) {
      parts.push_back(receiver);
      for (size_t i = 1; i < node.nodes.size(); i++)
        parts.push_back(print_postfix(*node.nodes[i]));
      return doc_concat(std::move(parts));
    }
    std::vector<DocP> cont;
    for (size_t i = 1; i < node.nodes.size(); i++) {
      if (is_dot(*node.nodes[i])) cont.push_back(doc_softline());  // break before `.`
      cont.push_back(print_postfix(*node.nodes[i]));
    }
    return doc_group(doc_concat({receiver, doc_indent(kIndent, doc_concat(std::move(cont)))}));
  }

  DocP print_assignment(const peg::Ast& node) {
    auto v = view_assignment(node);
    std::vector<DocP> parts;
    if (v.is_let) parts.push_back(doc_text("let "));
    if (v.is_mut) parts.push_back(doc_text("mut "));
    // lvalue chain: children [lvaloff .. lvaloff + lvalcnt). First is the
    // PRIMARY target, the rest are postfix segments.
    parts.push_back(print(*node.nodes[v.lvaloff]));
    for (int k = 1; k < v.lvalcnt; k++)
      parts.push_back(print_postfix(*node.nodes[v.lvaloff + k]));
    if (!v.type_annotation.empty())
      parts.push_back(doc_text(": " + std::string(v.type_annotation)));
    parts.push_back(doc_text(" " + std::string(v.op_token) + " "));
    parts.push_back(print(*v.rhs));
    return doc_concat(std::move(parts));
  }

  DocP print_array(const peg::Ast& node) {
    // ARRAY children: SEQUENCE (a kept node) holding SEQ_ELEM/SPREAD_ELEM, or
    // empty. A trailing `(rows, cols)` tensor-shape form is rare -> verbatim.
    if (node.nodes.empty()) return doc_text("[]");
    if (node.nodes.size() != 1 || node.nodes[0]->name != "SEQUENCE")
      return verbatim(node);
    std::vector<DocP> items;
    for (auto& e : node.nodes[0]->nodes) items.push_back(print_elem(*e));
    return print_delimited("[", std::move(items), "]");
  }

  DocP print_elem(const peg::Ast& e) {
    if (e.original_name == "SPREAD_ELEM")
      return doc_concat({doc_text("..."), print(*e.nodes[0])});
    return print(e);
  }

  DocP print_set(const peg::Ast& node) {
    std::vector<DocP> items;
    for (auto& e : node.nodes) items.push_back(print(*e));
    return print_delimited("{", std::move(items), "}");
  }

  DocP print_tuple(const peg::Ast& node) {
    std::vector<DocP> items;
    for (auto& e : node.nodes) items.push_back(print(*e));
    // A 1-tuple must keep its trailing comma (`(a,)`); print_delimited would
    // drop it and turn the tuple into a plain parenthesized expression.
    if (items.size() == 1)
      return doc_concat({doc_text("("), items[0], doc_text(",)")});
    return print_delimited("(", std::move(items), ")");
  }

  DocP print_object(const peg::Ast& node) {
    if (node.nodes.empty()) return doc_text("{}");
    std::vector<DocP> items;
    for (auto& p : node.nodes) {
      if (p->original_name == "SPREAD_ELEM") {
        items.push_back(doc_concat({doc_text("..."), print(*p->nodes[0])}));
        continue;
      }
      auto ov = view_object_property(*p);
      std::string key(ov.key->name == "STRING" || ov.key->name == "INTERPOLATED_STRING"
                          ? slice(*ov.key)
                          : std::string(ov.key->token));
      // Non-identifier literal keys slice verbatim; identifiers use the token.
      DocP keyd = (ov.key->name == "IDENTIFIER") ? doc_text(std::string(ov.key->token))
                                                  : verbatim(*ov.key);
      DocP muts = ov.is_mut ? doc_text("mut ") : doc_text("");
      if (ov.is_shorthand)
        items.push_back(doc_concat({muts, keyd}));
      else
        items.push_back(doc_concat({muts, keyd, doc_text(": "), print(*ov.value)}));
    }
    return print_delimited("{", std::move(items), "}");
  }

  // A destructuring pattern: containers (`(...)` / `[...]` / `{...}`) get
  // normalized comma spacing; leaf patterns (identifier, `_`, `name: Type`,
  // `Ctor(a)`, literals, `a | b` alternation) are sliced verbatim.
  DocP print_pattern(const peg::Ast& n) {
    // FOR_BINDING (multi-target `for k, v in …`) shares the tuple pattern's
    // shape; render it parenthesized like a tuple (matching `for (k, v)`).
    if (n.name == "TUPLE_PATTERN" || n.name == "FOR_BINDING") {
      std::vector<DocP> items;
      for (auto& c : n.nodes) items.push_back(print_pattern(*c));
      if (items.size() == 1)
        return doc_concat({doc_text("("), items[0], doc_text(",)")});
      return print_delimited("(", std::move(items), ")");
    }
    if (n.name == "ARRAY_PATTERN") {
      std::vector<DocP> items;
      for (auto& c : n.nodes) items.push_back(print_pattern(*c));  // REST falls to verbatim
      return print_delimited("[", std::move(items), "]");
    }
    if (n.name == "OBJECT_PATTERN") {
      std::vector<DocP> items;
      for (auto& e : n.nodes) {
        if (e->name == "OBJECT_PAT_ENTRY" && e->nodes.size() == 2)  // `key: pattern`
          items.push_back(doc_concat({doc_text(std::string(e->nodes[0]->token) + ": "),
                                      print_pattern(*e->nodes[1])}));
        else
          items.push_back(verbatim(*e));  // shorthand `name`
      }
      return print_delimited("{", std::move(items), "}");
    }
    if (n.name == "CTOR_PATTERN") {
      // [CTOR_PATH, PATTERN*] — `Rect(w, h)`, `Result.Ok(x)`.
      std::vector<DocP> items;
      for (size_t i = 1; i < n.nodes.size(); i++) items.push_back(print_pattern(*n.nodes[i]));
      return doc_concat({doc_text(std::string(n.nodes[0]->token)),
                         print_delimited("(", std::move(items), ")")});
    }
    return verbatim(n);
  }

  DocP print_param(const peg::Ast& p) {
    auto v = view_parameter(p);
    if (v.is_kw_only_sep) return doc_text("*");
    if (v.is_args_rest) return doc_text("*" + std::string(v.name));
    if (v.is_kwargs_rest) return doc_text("**" + std::string(v.name));
    if (v.pattern) return print_pattern(*v.pattern);
    DocP d = doc_text((v.is_mut ? "mut " : "") + std::string(v.name));
    if (!v.type_annotation.empty())
      d = doc_concat({d, doc_text(": " + std::string(v.type_annotation))});
    if (v.default_value) {
      // A top-level bit-or in a default must keep its parens — a bare `|` would
      // close the `|...|` lambda (and param defaults parse on the no-bit-or
      // grammar ladder, so this is the one operator precedence can't recover).
      DocP dv = print(*v.default_value);
      if (v.default_value->name == "BIT_OR")
        dv = doc_concat({doc_text("("), dv, doc_text(")")});
      d = doc_concat({d, doc_text(" = "), dv});
    }
    return d;
  }

  DocP print_params(const peg::Ast& params) {
    std::vector<DocP> items;
    for (auto& p : params.nodes) items.push_back(print_param(*p));
    return print_delimited("(", std::move(items), ")");
  }

  DocP print_destructure(const peg::Ast& node) {
    // [LET, MUTABLE, PATTERN, EXPRESSION]
    std::vector<DocP> parts;
    if (node.nodes[0]->token == "let") parts.push_back(doc_text("let "));
    if (node.nodes[1]->token == "mut") parts.push_back(doc_text("mut "));
    parts.push_back(print_pattern(*node.nodes[2]));
    parts.push_back(doc_text(" = "));
    parts.push_back(print(*node.nodes[3]));
    return doc_concat(std::move(parts));
  }

  DocP print_import(const peg::Ast& node) {
    // [IDENTIFIER, STRING]
    return doc_text("import " + std::string(node.nodes[0]->token) + " from " +
                    slice(*node.nodes[1]));
  }

  DocP print_export(const peg::Ast& node) {
    // `export { name, ... }`
    std::string out = "export {";
    for (size_t i = 0; i < node.nodes.size(); i++) {
      if (i) out += ", ";
      out += std::string(node.nodes[i]->token);
    }
    out += "}";
    return doc_text(out);
  }

  DocP print_function(const peg::Ast& node, bool named) {
    // named (MULTIFN_DECL): [DECORATOR*, CLASS_HEAD, PARAMETERS, RETURN_TYPE?, BLOCK]
    // anon  (FUNCTION):     [PARAMETERS, RETURN_TYPE?, BLOCK]
    std::vector<DocP> parts;
    size_t i = 0;
    if (named) {
      while (i < node.nodes.size() && node.nodes[i]->original_name == "DECORATOR") {
        parts.push_back(doc_concat({doc_text("@"), print(*node.nodes[i]->nodes[0])}));
        parts.push_back(doc_hardline());
        i++;
      }
      parts.push_back(doc_text("fn "));
      parts.push_back(doc_text(std::string(node.nodes[i]->token)));  // CLASS_HEAD
      i++;
    } else {
      parts.push_back(doc_text("fn "));
    }
    parts.push_back(print_params(*node.nodes[i]));  // PARAMETERS
    i++;
    if (i < node.nodes.size() && node.nodes[i]->original_name == "RETURN_TYPE") {
      parts.push_back(doc_text(" -> " + slice_inner(*node.nodes[i])));
      i++;
    }
    parts.push_back(doc_text(" "));
    parts.push_back(print_block(*node.nodes[i], node_end(*node.nodes[i - 1]),
                                /*empty_ok=*/true));
    return doc_concat(std::move(parts));
  }

  // RETURN_TYPE / TYPE_ANNOTATION carry their captured type token; emit it.
  std::string slice_inner(const peg::Ast& a) const {
    return std::string(a.token.empty() ? slice(a) : std::string(a.token));
  }

  DocP print_lambda(const peg::Ast& node) {
    // [LAMBDA_PARAMS, EXPRESSION]
    std::vector<DocP> items;
    for (auto& p : node.nodes[0]->nodes) items.push_back(print_param(*p));
    std::vector<DocP> parts;
    parts.push_back(doc_text("|"));
    for (size_t k = 0; k < items.size(); k++) {
      parts.push_back(items[k]);
      if (k + 1 < items.size()) parts.push_back(doc_text(", "));
    }
    parts.push_back(doc_text("| "));
    parts.push_back(print(*node.nodes[1]));
    return doc_concat(std::move(parts));
  }

  DocP print_if(const peg::Ast& node) {
    // [(INIT_CLAUSE)?, cond, then, (cond, block)*, (block)?]. The optional init
    // clause renders as `binding, binding; ` before the first condition; the
    // cond/block pairing then starts at arm_off. Each block's `{` is located by
    // scanning from the preceding child's end (cond) or the previous block's
    // close (the else block, whose `{` follows `else`).
    auto iv = culebra::view_if(node);
    const auto& nodes = node.nodes;
    size_t off = iv.arm_off;
    std::vector<DocP> parts;
    parts.push_back(doc_text("if "));
    if (iv.init) {
      for (size_t k = 0; k < iv.init->nodes.size(); k++) {
        if (k) parts.push_back(doc_text(", "));
        parts.push_back(print(*iv.init->nodes[k]));
      }
      parts.push_back(doc_text("; "));
    }
    parts.push_back(print(*nodes[off]));
    parts.push_back(doc_text(" "));
    parts.push_back(print_block(*nodes[off + 1], node_end(*nodes[off]), false));
    size_t cursor = block_interior(node_end(*nodes[off])).after;
    size_t i = off + 2, n = nodes.size();
    while (n - i >= 2) {
      parts.push_back(doc_text(" else if "));
      parts.push_back(print(*nodes[i]));
      parts.push_back(doc_text(" "));
      parts.push_back(print_block(*nodes[i + 1], node_end(*nodes[i]), false));
      cursor = block_interior(node_end(*nodes[i])).after;
      i += 2;
    }
    if (i < n) {
      parts.push_back(doc_text(" else "));
      parts.push_back(print_block(*nodes[i], cursor, false));
    }
    return doc_concat(std::move(parts));
  }

  // Render a loop's optional trailing ` nobreak { … }` clause, or nothing.
  // `after` is the node whose end the nobreak block's `{` follows (the body).
  DocP print_nobreak(const peg::Ast* nobreak, const peg::Ast& after) {
    if (!nobreak) return doc_text("");
    return doc_concat({doc_text(" nobreak "),
                       print_block(*nobreak, node_end(after), false)});
  }

  DocP print_while(const peg::Ast& node) {
    // [(INIT_CLAUSE)?, condition, BLOCK, (NOBREAK_CLAUSE)?]. The optional init
    // clause renders as `binding, binding; ` before the condition
    // (`while mut i = 0; i < n {…}`); a nobreak clause renders after the body.
    auto wv = culebra::view_while(node);
    DocP head = print(*wv.cond);
    if (wv.init) {
      std::vector<DocP> parts;
      for (size_t i = 0; i < wv.init->nodes.size(); i++) {
        if (i) parts.push_back(doc_text(", "));
        parts.push_back(print(*wv.init->nodes[i]));
      }
      parts.push_back(doc_text("; "));
      parts.push_back(head);
      head = doc_concat(std::move(parts));
    }
    return doc_concat({doc_text("while "), head, doc_text(" "),
                       print_block(*wv.body, node_end(*wv.cond), false),
                       print_nobreak(wv.nobreak, *wv.body)});
  }

  DocP print_for(const peg::Ast& node) {
    // [binding, iter, BLOCK, (NOBREAK_CLAUSE)?]. The binding is a single var or
    // a pattern; a nobreak clause renders after the body.
    auto fv = culebra::view_for(node);
    return doc_concat({doc_text("for "), print_pattern(*fv.binding),
                       doc_text(" in "), print(*fv.iter), doc_text(" "),
                       print_block(*fv.body, node_end(*fv.iter), false),
                       print_nobreak(fv.nobreak, *fv.body)});
  }

  DocP print_defer(const peg::Ast& node) {
    return doc_concat({doc_text("defer "),
                       print_block(*node.nodes[0], node.position, true)});
  }

  DocP print_try(const peg::Ast& node) {
    // [BLOCK, IDENTIFIER, BLOCK]. The try block's `{` follows `try`; the catch
    // block's `{` follows the bound identifier.
    return doc_concat({doc_text("try "),
                       print_block(*node.nodes[0], node.position, false),
                       doc_text(" catch " + std::string(node.nodes[1]->token) + " "),
                       print_block(*node.nodes[2], node_end(*node.nodes[1]), false)});
  }

  DocP print_keyword_expr(const std::string& kw, const peg::Ast& node) {
    // RETURN / THROW / YIELD: keyword with optional trailing expression.
    if (node.nodes.empty()) return doc_text(kw);
    return doc_concat({doc_text(kw + " "), print(*node.nodes[0])});
  }

  // A match / cond arm body is `(EXPRESSION | BLOCK)`. A brace block lays out
  // multi-line; an expression body stays on the `=>` line.
  static bool is_block_body(const peg::Ast& b) { return b.original_name == "BLOCK"; }
  DocP print_arm_body(const peg::Ast& body, size_t after_pos) {
    if (is_block_body(body)) return print_block(body, after_pos, /*empty_ok=*/true);
    return print(body);
  }

  DocP print_match(const peg::Ast& node) {
    // [(INIT_CLAUSE)?, subject, MATCH_ARMS]. MATCH_ARM = [PATTERN, GUARD?, body].
    // The optional init clause renders as `binding, binding; ` before the
    // subject (`match mut x = f(); x { … }`).
    auto mv = culebra::view_match(node);
    const peg::Ast& arms = *mv.arms;
    std::vector<const peg::Ast*> items;
    for (auto& a : arms.nodes) items.push_back(a.get());
    auto render = [&](size_t k) {
      const peg::Ast& arm = *items[k];
      bool guard = arm.nodes.size() >= 2 && arm.nodes[1]->original_name == "GUARD";
      const peg::Ast& body = *arm.nodes.back();
      std::vector<DocP> parts;
      parts.push_back(print_pattern(*arm.nodes[0]));  // normalize pattern spacing
      if (guard)
        parts.push_back(doc_concat({doc_text(" if "), print(*arm.nodes[1]->nodes[0])}));
      parts.push_back(doc_text(" => "));
      parts.push_back(print_arm_body(body, node_end(*arm.nodes[arm.nodes.size() - 2])));
      return doc_concat(std::move(parts));
    };
    std::vector<DocP> head;
    head.push_back(doc_text("match "));
    if (mv.init) {
      for (size_t k = 0; k < mv.init->nodes.size(); k++) {
        if (k) head.push_back(doc_text(", "));
        head.push_back(print(*mv.init->nodes[k]));
      }
      head.push_back(doc_text("; "));
    }
    head.push_back(print(*mv.subject));
    head.push_back(doc_text(" "));
    head.push_back(print_braced(items, node_end(*mv.subject), ",",
                                /*empty_ok=*/true, render));
    return doc_concat(std::move(head));
  }

  DocP print_cond(const peg::Ast& node) {
    // [COND_ARM*]. COND_ARM = [(WILDCARD | EXPRESSION), body].
    std::vector<const peg::Ast*> items;
    for (auto& a : node.nodes) items.push_back(a.get());
    auto render = [&](size_t k) {
      const peg::Ast& arm = *items[k];
      const peg::Ast& test = *arm.nodes[0];
      DocP testd = test.name == "WILDCARD" ? doc_text("_") : print(test);
      return doc_concat({testd, doc_text(" => "),
                         print_arm_body(*arm.nodes[1], node_end(test))});
    };
    return doc_concat({doc_text("cond "),
                       print_braced(items, node.position, ",", /*empty_ok=*/true, render)});
  }

  // Leading `@deco` children shared by class / trait / enum / fn declarations:
  // emit each on its own line and return the index of the first real child.
  size_t emit_decorators(const peg::Ast& node, std::vector<DocP>& parts) {
    size_t i = 0;
    while (i < node.nodes.size() && node.nodes[i]->original_name == "DECORATOR") {
      parts.push_back(doc_concat({doc_text("@"), print(*node.nodes[i]->nodes[0])}));
      parts.push_back(doc_hardline());
      i++;
    }
    return i;
  }

  DocP print_method(const peg::Ast& m) {
    auto v = view_method(m);
    if (v.is_field)  // `static x = expr`
      return doc_concat({doc_text("static " + std::string(v.name) + " = "),
                         print(*v.value)});
    // `static` and `get` are mutually exclusive member modifiers.
    DocP prefix = v.is_static ? doc_text("static ")
                : v.is_getter ? doc_text("get ")
                              : doc_text("");
    if (v.is_typed_field) {
      DocP d = doc_concat({prefix, doc_text(std::string(v.name) + ": " +
                                            std::string(v.type_annotation))});
      if (v.value) d = doc_concat({d, doc_text(" = "), print(*v.value)});
      return d;
    }
    return doc_concat({prefix, doc_text(std::string(v.name)), print_params(*v.params),
                       doc_text(" "),
                       print_block(**v.body, node_end(*v.params), /*empty_ok=*/true)});
  }

  DocP print_class(const peg::Ast& node) {
    // [DECORATOR*, CLASS_HEAD, METHOD*]
    std::vector<DocP> parts;
    size_t i = emit_decorators(node, parts);
    const peg::Ast& head = *node.nodes[i++];
    std::vector<const peg::Ast*> members;
    for (size_t j = i; j < node.nodes.size(); j++) members.push_back(node.nodes[j].get());
    parts.push_back(doc_text("class " + std::string(head.token) + " "));
    parts.push_back(print_braced(members, node_end(head), "", /*empty_ok=*/true,
                                 [&](size_t k) { return print_method(*members[k]); }));
    return doc_concat(std::move(parts));
  }

  DocP print_trait(const peg::Ast& node) {
    // [DECORATOR*, TRAIT_HEAD, TRAIT_METHOD*]. A method is signature-only or has
    // a default-impl block.
    std::vector<DocP> parts;
    size_t i = emit_decorators(node, parts);
    const peg::Ast& head = *node.nodes[i++];
    std::vector<const peg::Ast*> methods;
    for (size_t j = i; j < node.nodes.size(); j++) methods.push_back(node.nodes[j].get());
    auto render = [&](size_t k) {
      auto v = view_trait_method(*methods[k]);
      DocP d = doc_concat({doc_text(std::string(v.name)), print_params(*v.params)});
      if (!v.return_type.empty())
        d = doc_concat({d, doc_text(" -> " + std::string(v.return_type))});
      if (v.body)
        d = doc_concat({d, doc_text(" "),
                        print_block(*v.body, node_end(*v.params), /*empty_ok=*/true)});
      return d;
    };
    parts.push_back(doc_text("trait " + std::string(head.token) + " "));
    parts.push_back(print_braced(methods, node_end(head), "", /*empty_ok=*/true, render));
    return doc_concat(std::move(parts));
  }

  DocP print_enum(const peg::Ast& node) {
    // [DECORATOR*, CLASS_HEAD, VARIANT*]. VARIANT = [IDENTIFIER, VARIANT_FIELD*].
    std::vector<DocP> parts;
    size_t i = emit_decorators(node, parts);
    const peg::Ast& head = *node.nodes[i++];
    std::vector<const peg::Ast*> variants;
    for (size_t j = i; j < node.nodes.size(); j++) variants.push_back(node.nodes[j].get());
    auto render = [&](size_t k) {
      const peg::Ast& v = *variants[k];
      std::string out(v.nodes[0]->token);
      if (v.nodes.size() > 1) {
        out += "(";
        for (size_t j = 1; j < v.nodes.size(); j++) {
          if (j > 1) out += ", ";
          out += std::string(v.nodes[j]->token);
        }
        out += ")";
      }
      return doc_text(out);
    };
    parts.push_back(doc_text("enum " + std::string(head.token) + " "));
    parts.push_back(print_braced(variants, node_end(head), ",", /*empty_ok=*/true, render));
    return doc_concat(std::move(parts));
  }

 public:
  DocP print(const peg::Ast& node) {
    const std::string& n = node.name;

    // Statements / control flow
    if (n == "STATEMENTS") return print_block(node, node.position, false);
    if (n == "LEXICAL_SCOPE") return print_lexical_scope(node);
    if (n == "ASSIGNMENT") return print_assignment(node);
    if (n == "MULTIFN_DECL") return print_function(node, /*named=*/true);
    if (n == "FUNCTION") return print_function(node, /*named=*/false);
    if (n == "LAMBDA") return print_lambda(node);
    if (n == "IF") return print_if(node);
    if (n == "WHILE") return print_while(node);
    if (n == "FOR") return print_for(node);
    if (n == "DEFER") return print_defer(node);
    if (n == "TRY") return print_try(node);
    if (n == "MATCH") return print_match(node);
    if (n == "COND") return print_cond(node);
    if (n == "CLASS_DECL") return print_class(node);
    if (n == "TRAIT_DECL") return print_trait(node);
    if (n == "ENUM_DECL") return print_enum(node);
    if (n == "DESTRUCTURE_ASSIGN") return print_destructure(node);
    if (n == "IMPORT_STMT") return print_import(node);
    if (n == "EXPORT_STMT") return print_export(node);
    if (n == "RETURN") return print_keyword_expr("return", node);
    if (n == "THROW") return print_keyword_expr("throw", node);
    if (n == "YIELD") return print_keyword_expr("yield", node);

    // Expressions
    if (n == "CALL") return print_call(node);
    if (n == "CONDITIONAL") return print_ternary(node);
    if (n == "RANGE") return print_range(node);
    if (is_unary(n)) return print_unary(node);
    if (is_binary_explicit(n) || is_binary_implicit(n)) return print_binary(node);
    if (n == "ARRAY") return print_array(node);
    if (n == "OBJECT") return print_object(node);
    if (n == "TUPLE") return print_tuple(node);
    if (n == "SET") return print_set(node);

    // Everything not yet handled: emit the node's source verbatim, keeping it
    // exact. Leaf atoms and wrapper-collapsed nodes (a string as a lone block
    // statement, an indexed atom) shed the stray `{}` / `[]` / `()` the
    // optimizer folded into their span; genuine constructs whose span legibly
    // begins/ends with a bracket but is not a wrapper (a destructuring assign
    // `(a, b) = ...`, a match) slice exactly. The safety re-parse guards any
    // residual mistake.
    if (node.is_token || is_wrapper_collapsed(node))
      return doc_text(std::string(tight_span(node)));
    return verbatim(node);
  }
};

// ----------------------------------------------------------------------------
// Driver
// ----------------------------------------------------------------------------

enum class FormatStatus { Ok, Unchanged, ParseError, Refused };

struct FormatResult {
  FormatStatus status;
  std::string output;   // valid when Ok / Unchanged
  std::string message;  // diagnostic for ParseError / Refused
};

// Multiset of comment texts (sorted), so two sources can be compared for
// comment preservation regardless of where the comments moved.
inline std::vector<std::string> comment_multiset(std::string_view s) {
  std::vector<std::string> out;
  for (const auto& c : scan_comments(s))
    out.emplace_back(s.substr(c.start, c.end - c.start));
  std::sort(out.begin(), out.end());
  return out;
}

inline FormatResult format_source(const std::string& path, std::string_view src,
                                  int width = 80) {
  std::vector<std::string> msgs;
  auto ast = parse_for_format(path, src.data(), src.size(), msgs);
  if (!ast) {
    std::string m;
    for (auto& s : msgs) m += s;
    return {FormatStatus::ParseError, "", m};
  }

  Printer printer(src);
  std::string out = doc_render(printer.print_program(*ast), width);
  if (out.empty() || out.back() != '\n') out += '\n';

  // Safety net 1: re-parse and require structural AST equality, so a printer
  // bug can never silently change program meaning.
  std::vector<std::string> msgs2;
  auto ast2 = parse_for_format(path, out.data(), out.size(), msgs2);
  if (!ast2 || !ast_equal(*ast, *ast2)) {
    return {FormatStatus::Refused, "",
            "culebra fmt: internal check failed (formatted output would change "
            "program meaning); left unchanged"};
  }

  // Safety net 2: comments are absent from the AST, so AST equality can't catch
  // a dropped or duplicated comment. Require the comment multiset to match.
  if (comment_multiset(src) != comment_multiset(out)) {
    return {FormatStatus::Refused, "",
            "culebra fmt: internal check failed (a comment would be dropped or "
            "moved incorrectly); left unchanged"};
  }

  if (out == src) return {FormatStatus::Unchanged, out, ""};
  return {FormatStatus::Ok, out, ""};
}

}  // namespace culebra::fmt
