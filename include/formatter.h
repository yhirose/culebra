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

enum class DocKind { Text, Line, SoftLine, HardLine, Group, Indent, Concat, IfBreak };

struct Doc {
  DocKind kind;
  std::string text;            // Text
  std::vector<DocP> children;  // Concat, Group (size 1), Indent (size 1)
  int indent = 0;              // Indent
  bool weightless = false;     // Text that the fit check ignores
  bool has_hardline = false;   // this doc or anything under it breaks
};

inline DocP doc_text(std::string s) {
  auto d = std::make_shared<Doc>();
  d->kind = DocKind::Text;
  d->text = std::move(s);
  return d;
}

// Text that occupies no width as far as layout is concerned. A trailing
// comment sits past the end of the code, so letting it count would break the
// code around it to make room for prose that follows either way.
inline DocP doc_weightless_text(std::string s) {
  auto d = doc_text(std::move(s));
  d->weightless = true;
  return d;
}
// Emitted only when the enclosing group breaks — a trailing comma that appears
// exactly when the list is laid out one element per line.
inline DocP doc_if_break(std::string s) {
  auto d = std::make_shared<Doc>();
  d->kind = DocKind::IfBreak;
  d->text = std::move(s);
  return d;
}
inline DocP doc_line() { auto d = std::make_shared<Doc>(); d->kind = DocKind::Line; return d; }
inline DocP doc_softline() { auto d = std::make_shared<Doc>(); d->kind = DocKind::SoftLine; return d; }
inline DocP doc_hardline() {
  auto d = std::make_shared<Doc>();
  d->kind = DocKind::HardLine;
  d->has_hardline = true;
  return d;
}
inline DocP doc_concat(std::vector<DocP> xs) {
  auto d = std::make_shared<Doc>();
  d->kind = DocKind::Concat;
  d->children = std::move(xs);
  for (const auto& c : d->children) d->has_hardline |= c && c->has_hardline;
  return d;
}
// A Group may only wrap docs that can lay out flat, so one holding a HardLine
// is no Group at all: doc_fits stops measuring at the first hardline and
// reports "fits", which would render everything past it as if flat. Degrading
// to the child is what break mode already renders — with no Group left on the
// path, the separators below lay out broken. Callers that want a specific
// shape for such a doc (print_delimited's hug, print_call's flat chain) test
// for the break themselves and never get here; this keeps the invariant true
// for the ones that don't.
inline DocP doc_group(DocP x) {
  if (x && x->has_hardline) return x;
  auto d = std::make_shared<Doc>();
  d->kind = DocKind::Group;
  d->children = {std::move(x)};
  return d;
}
inline DocP doc_indent(int n, DocP x) {
  auto d = std::make_shared<Doc>();
  d->kind = DocKind::Indent;
  d->indent = n;
  d->has_hardline = x && x->has_hardline;
  d->children = {std::move(x)};
  return d;
}

// Force a doc to its flat form. A head with no brackets of its own — the
// condition of an `if` / `while`, the subject of a `match` — has nothing to
// break *inside*, so a break lands between two operands (`if v ==` / `0 {`) and
// reads as a mistake. Width pressure from the rest of the line must not reach
// it.
inline DocP doc_flatten(const DocP& d) {
  if (!d) return d;
  if (d->kind == DocKind::Line) return doc_text(" ");
  if (d->kind == DocKind::SoftLine || d->kind == DocKind::IfBreak) return doc_text("");
  if (d->children.empty()) return d;
  auto c = std::make_shared<Doc>(*d);
  for (auto& ch : c->children) ch = doc_flatten(ch);
  return c;
}

// Whether a doc breaks a line no matter how much room it is given. Folded in
// at construction, so asking is O(1) however deeply the doc is nested.
inline bool doc_has_hardline(const DocP& d) { return d && d->has_hardline; }

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
        if (!c.doc->weightless) remaining -= static_cast<int>(c.doc->text.size());
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
      case DocKind::IfBreak:
        if (!c.flat) remaining -= static_cast<int>(c.doc->text.size());
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
  // Indentation is owed, not written: a line that never receives text stays
  // empty instead of collecting trailing spaces.
  int pending_indent = -1;
  auto put = [&](std::string_view s) {
    if (s.empty()) return;
    if (pending_indent >= 0) { out.append(pending_indent, ' '); pending_indent = -1; }
    out += s;
  };
  auto newline = [&](int indent) {
    out += '\n';
    pending_indent = indent;
    col = indent;
  };
  std::vector<LayoutCmd> stack = {{0, /*flat=*/false, root.get()}};
  while (!stack.empty()) {
    LayoutCmd c = stack.back();
    stack.pop_back();
    switch (c.doc->kind) {
      case DocKind::Text:
        put(c.doc->text);
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
        if (c.flat) { put(" "); col += 1; }
        else { newline(c.indent); }
        break;
      case DocKind::IfBreak:
        if (!c.flat) { put(c.doc->text); col += static_cast<int>(c.doc->text.size()); }
        break;
      case DocKind::SoftLine:
        if (!c.flat) { newline(c.indent); }
        break;
      case DocKind::HardLine:
        newline(c.indent);
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
    index_comment_owners();
  }

  DocP print_program(const peg::Ast& program) {
    // The file is the top-level statement list, which owns every comment
    // outside all braces — the interior key 0 (see owner_).
    return print_statement_list(stmt_children(program), 0);
  }

 private:
  std::string_view src_;
  std::vector<Comment> comments_;  // sorted by start (scan order)
  std::vector<char> is_code_;      // per-byte code mask (see scan_source)
  // Comment ownership, parallel to comments_: the byte just past the `{` of the
  // innermost code-level brace pair holding the comment, 0 when none does.
  // Printers get the same key from block_interior() (same mask, same matching),
  // so a list owns a comment exactly when owner_[ci] equals its interior start.
  // Object and set literals open a pair of their own, so their interior
  // comments are never mistaken for a statement's.
  std::vector<size_t> owner_;
  static constexpr int kIndent = 2;

  // A comment byte is never code (the scanner clears the mask over it), so
  // braces written inside comments — and inside strings / interpolation holes —
  // are ignored here just as block_interior() ignores them.
  void index_comment_owners() {
    owner_.assign(comments_.size(), 0);
    std::vector<size_t> open;  // interiors of the brace pairs still open
    size_t ci = 0;
    for (size_t i = 0; i < src_.size(); i++) {
      if (ci < comments_.size() && comments_[ci].start == i)
        owner_[ci++] = open.empty() ? 0 : open.back();
      if (!is_code_[i]) continue;
      if (src_[i] == '{') open.push_back(i + 1);
      else if (src_[i] == '}' && !open.empty()) open.pop_back();
    }
  }

  std::string slice(const peg::Ast& a) const {
    return std::string(src_.substr(a.position, a.length));
  }
  DocP verbatim(const peg::Ast& a) const { return doc_text(slice(a)); }

  // True iff the leading bracket of `t` is closed by the bracket at its very
  // end (so the pair encloses the whole span) rather than by some interior
  // bracket — `(a) = (b)` must NOT be treated as a wrapped `a) = (b`. Only code
  // bytes count: a bracket inside a comment or a string would otherwise
  // unbalance the pair, leaving the wrapper on and the comment it wrapped
  // duplicated between the leaf and the enclosing list.
  bool encloses_whole(std::string_view t, char open, char close) const {
    size_t off = static_cast<size_t>(t.data() - src_.data());
    int depth = 0;
    for (size_t i = 0; i < t.size(); i++) {
      if (!is_code_[off + i]) continue;
      if (t[i] == open) depth++;
      else if (t[i] == close) { if (--depth == 0) return i == t.size() - 1; }
    }
    return false;
  }

  // A leaf atom (IDENTIFIER / NUMBER / STRING / ...) carries an exact token, but
  // the AstOptimizer may have collapsed a delimiter-adding wrapper onto it (a
  // single-statement block `{ x }`, a parenthesized atom `( x )`), widening the
  // node's span onto those brackets while its `.token` drops any string quotes.
  // Recover the node's own text by stripping the balanced wrapper brackets and
  // surrounding whitespace. A node's intrinsic delimiters are never shed: a
  // string's quotes are not a bracket pair, and every bracket/brace literal
  // (array / object / tuple / set) is printed explicitly and never reaches here.
  std::string_view tight_span(const peg::Ast& a) const {
    std::string_view t = src_.substr(a.position, a.length);
    // A comment the optimizer folded into the span goes with the brackets it
    // came wrapped in. `fn f() { # why \n 1 }` collapses BLOCK onto the NUMBER
    // leaf, so the span carries the body's comment; the enclosing statement
    // list also emits that comment (it sits before the item's first token), so
    // leaving it here duplicates it and the preservation net refuses the file.
    // Only a whole comment at either edge is shed — one *between* tokens is not
    // the wrapper's, and the caller keeps the span verbatim to preserve it.
    auto shed_edge_comment = [&] {
      size_t off = static_cast<size_t>(t.data() - src_.data());
      for (const auto& c : comments_) {
        if (c.start == off && c.end <= off + t.size()) {
          t.remove_prefix(c.end - c.start);
          return true;
        }
        if (c.end == off + t.size() && c.start >= off) {
          t.remove_suffix(c.end - c.start);
          return true;
        }
      }
      return false;
    };
    for (;;) {
      while (!t.empty() && (t.front() == ' ' || t.front() == '\t' ||
                            t.front() == '\n' || t.front() == '\r'))
        t.remove_prefix(1);
      while (!t.empty() && (t.back() == ' ' || t.back() == '\t' ||
                            t.back() == '\n' || t.back() == '\r'))
        t.remove_suffix(1);
      if (shed_edge_comment()) continue;
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
    // A lambda's body runs as far right as it can, so it is never a primary
    // and needs parentheses as an operand of anything -- `(|q| f(q))(x)`
    // reprinted without them is a lambda whose body is `f(q)(x)`. The
    // default below is 16 (binds tightest), which is the wrong answer here.
    if (name == "LAMBDA") return 0;
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
  // An operator chain carries no brackets of its own, so a break inside it has
  // nothing to explain it — see print_condition.
  static bool is_bare_chain(const std::string& n) {
    return is_binary_explicit(n) || is_binary_implicit(n) || n == "CONDITIONAL";
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

  // The first brace block at or after `from`: returns {lo = just after `{`,
  // after = just past the matching `}`}. `lo` doubles as the block's identity
  // for comment ownership (see owner_), since it is derived from the same
  // is_code_ brace matching. The scan ignores braces inside strings /
  // interpolation holes, and balances object / set / nested-block braces.
  // `found` is false if no block brace exists (shouldn't happen for a real
  // block).
  struct BlockSpan { size_t lo, after; bool found; };
  BlockSpan block_interior(size_t from) const {
    size_t i = from, n = src_.size();
    while (i < n && !(is_code_[i] && src_[i] == '{')) i++;
    if (i >= n) return {from, from, false};
    size_t lo = i + 1;
    int depth = 0;
    for (; i < n; i++) {
      if (!is_code_[i]) continue;
      if (src_[i] == '{') depth++;
      else if (src_[i] == '}' && --depth == 0)
        return {lo, i + 1, true};
    }
    return {lo, n, false};
  }

  // Comment text as a single Doc line, trailing whitespace already trimmed by
  // the scanner. (Multi-line block comments keep their embedded newlines.)
  DocP comment_doc(const Comment& c) const {
    return doc_text(std::string(src_.substr(c.start, c.end - c.start)));
  }

  // Render the statement list whose brace interior begins at `lo` (0 for the
  // file itself). Comments this list owns are attached: an own-line comment
  // becomes a standalone line before the following statement; a same-line
  // comment trails the preceding statement. Comments owned by a nested pair
  // belong to whichever printer recurses into it; any comment nobody emits is
  // caught by the comment-preservation check.
  DocP print_statement_list(const std::vector<const peg::Ast*>& stmts, size_t lo) {
    return print_items(stmts, lo, "", [&](size_t k) { return print(*stmts[k]); });
  }

  // Render a comma/blank-separated list of member nodes whose brace interior
  // begins at `lo`, with the comments that interior owns attached. Shared by
  // statement blocks, match/cond arms, class members, and enum variants.
  // `render(k)` produces the body of item k (without the trailing `sep`); `sep`
  // is "" (statements/members) or "," (arms/variants), appended after each item
  // before any trailing comment.
  //
  // Which comments land here is settled by owner_ alone. What remains positional
  // is only layout: where within this list a comment goes, and which items must
  // fall back to a verbatim slice. That rests on three offsets per item, all of
  // which the AST optimizer's single-child span widening can corrupt (a lone
  // item inherits its block's span, swallowing the `{ }` and a leading comment):
  //   * extent — code-tight span (keyword..closing delimiter), for ordering and
  //     blank-line math;
  //   * head/tail — first/last descendant-token offset (never widened), to test
  //     whether a comment lies between an item's tokens;
  //   * first-token position — extent start past a swallowed `{` / comment, the
  //     sort key, so an item never sorts before its own leading comment.
  template <typename Render>
  DocP print_items(const std::vector<const peg::Ast*>& items, size_t lo,
                   const std::string& sep, Render render) {
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

    // A comment this list owns that falls between some item's first and last
    // token is already inside the bytes that item may slice verbatim, so the
    // list must not emit it a second time.
    auto inside_item = [&](size_t p) {
      for (size_t k = 0; k < items.size(); k++)
        if (head[k] <= p && p < tail[k]) return true;
      return false;
    };
    // An owned comment between item k's first and last token is one the item's
    // printer has no slot for (a wrapped chain's continuation line, say), so the
    // item is emitted verbatim to keep it. Comments a nested pair owns are not
    // this item's problem — the printer that recurses into that pair places
    // them, which is how a bare block's statements keep their own comments.
    auto has_mid_comment = [&](size_t k) {
      for (size_t ci = 0; ci < comments_.size(); ci++)
        if (owner_[ci] == lo && head[k] < comments_[ci].start &&
            comments_[ci].start < tail[k])
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

    // A wrapper-collapsed item (e.g. a TRY that is the sole statement of a
    // bare block) has its span widened onto the wrapper's own braces, so a
    // verbatim slice of `extent[k]` would still carry that outer `{`/`}` —
    // doubling up when this doc is placed inside the enclosing block's own
    // braces. Shed exactly that one collapsed brace layer first.
    auto verbatim_span = [&](size_t k) {
      size_t vs = extent[k].first, ve = extent[k].second;
      if (is_wrapper_collapsed(*items[k])) {
        std::string_view t = src_.substr(vs, ve - vs);
        if (t.size() >= 2 && t.front() == '{' && encloses_whole(t, '{', '}')) {
          vs++;
          ve--;
          while (vs < ve && (src_[vs] == ' ' || src_[vs] == '\t' ||
                             src_[vs] == '\n' || src_[vs] == '\r'))
            vs++;
          while (ve > vs && (src_[ve - 1] == ' ' || src_[ve - 1] == '\t' ||
                             src_[ve - 1] == '\n' || src_[ve - 1] == '\r'))
            ve--;
        }
      }
      return std::make_pair(vs, ve);
    };

    std::vector<DocP> trailing(items.size());
    for (size_t k = 0; k < items.size(); k++) {
      DocP doc;
      if (has_mid_comment(k)) {
        auto [vs, ve] = verbatim_span(k);
        // The slice runs to the item's closing delimiter, so it carries every
        // comment in those bytes — including one past the last token, inside
        // a `]`. Widening the item's reach is what stops the list from also
        // trailing that comment onto it. (has_mid_comment has already read
        // head/tail for this item; the readers left run after this loop.)
        head[k] = std::min(head[k], vs);
        tail[k] = std::max(tail[k], ve);
        doc = doc_text(std::string(src_.substr(vs, ve - vs)));
      } else {
        doc = render(k);
      }
      if (!sep.empty()) doc = doc_concat({doc, doc_text(sep)});
      entries.push_back({first_token_pos(k), extent[k].first, extent[k].second, doc});
    }

    for (size_t ci = 0; ci < comments_.size(); ci++) {
      const Comment& c = comments_[ci];
      if (owner_[ci] != lo) continue;      // a nested pair's printer places it
      if (inside_item(c.start)) continue;  // handled by the item's printer
      if (!c.own_line) {
        int pk = prev_item_index(c.start);
        if (pk >= 0) {
          // Weightless: the comment follows the code either way, so counting it
          // would only break the code in front of it to make room.
          DocP gap = doc_weightless_text("  ");
          DocP text = doc_weightless_text(
              std::string(src_.substr(c.start, c.end - c.start)));
          trailing[pk] = trailing[pk]
              ? doc_concat({trailing[pk], std::move(gap), std::move(text)})
              : doc_concat({std::move(gap), std::move(text)});
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

  bool owns_a_comment(size_t lo) const {
    for (size_t ci = 0; ci < comments_.size(); ci++)
      if (owner_[ci] == lo) return true;
    return false;
  }

  // A `{ ... }` brace group, always multi-line, holding `items` rendered by
  // `render` (with comments attached) and separated by `sep` ("" or ","). The
  // brace interior is located by scanning from `after_pos` (at or before `{`).
  template <typename Render>
  DocP print_braced(const std::vector<const peg::Ast*>& items, size_t after_pos,
                    const std::string& sep, bool empty_ok, Render render) {
    BlockSpan span = block_interior(after_pos);
    size_t lo = span.found ? span.lo : (items.empty() ? after_pos : items.front()->position);

    // With no items, print_items emits just the comments this interior owns —
    // which is the dangling-comment block. Only a truly empty one is special.
    if (items.empty() && !owns_a_comment(lo))
      return empty_ok ? doc_text("{}")
                      : doc_concat({doc_text("{"), doc_hardline(), doc_text("}")});
    return doc_concat({
        doc_text("{"),
        doc_indent(kIndent, doc_concat({doc_hardline(),
                                        print_items(items, lo, sep, render)})),
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
  // `parent_name` is the enclosing binary rule, when there is one.
  DocP print_operand(const peg::Ast& node, int parent_prec, bool assoc_safe,
                     std::string_view parent_name = {},
                     bool flat_when_bare = false) {
    int cp = prec(node.name);
    bool paren = cp < parent_prec || (cp == parent_prec && !assoc_safe);
    // Parentheses around an operand of the SAME rule stay even when
    // associativity makes them redundant: the grammar folds a run of
    // same-precedence operators into one flat node, so `(a * b) / c` and
    // `a * b / c` mean the same thing but are different trees. Dropping them
    // would reshape the AST, and the re-parse safety net would then refuse the
    // whole file rather than emit a differently-shaped program.
    if (!paren && cp == parent_prec && node.name == parent_name) paren = true;
    DocP d = print(node);
    if (paren) return doc_concat({doc_text("("), d, doc_text(")")});
    // In a condition, an operand that is itself a bare chain must stay flat: a
    // break there lands between two operands with nothing to explain it. One
    // with brackets of its own — a call, a parenthesised sub-expression —
    // carries its breaks inside them, where they read fine.
    if (flat_when_bare && is_bare_chain(node.name)) return doc_flatten(d);
    return d;
  }

  // `chain_only` keeps every operand that is itself a bare chain flat, leaving
  // this chain's own joints as the only bracket-free place a break can land.
  // It is print_condition's.
  DocP print_binary(const peg::Ast& node, bool chain_only = false) {
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
    std::vector<DocP> ods{print_operand(*operands[0], P,
                                       /*assoc_safe=*/!right_assoc, node.name,
                                       chain_only)};
    for (size_t k = 1; k < m; k++)
      ods.push_back(print_operand(*operands[k], P,
                                  right_assoc ? (k == m - 1) : false, node.name,
                                  chain_only));

    // An operand that breaks on its own — a call carrying a block argument —
    // leaves the chain nothing worth wrapping: the width is already spent, and
    // breaking after an operator would only push that operand's own lines a
    // level deeper. Keep the chain on one line and let the operand break
    // inside itself.
    if (std::any_of(ods.begin(), ods.end(), doc_has_hardline)) {
      std::vector<DocP> flat{ods[0]};
      for (size_t k = 1; k < m; k++) {
        flat.push_back(doc_text(" " + ops[k - 1] + " "));
        flat.push_back(std::move(ods[k]));
      }
      return doc_concat(std::move(flat));
    }

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
      cont.push_back(doc_text(" " + ops[k - 1]));
      cont.push_back(doc_line());
      cont.push_back(std::move(ods[k]));
    }
    return doc_group(doc_concat({
        std::move(ods[0]),
        doc_indent(kIndent, doc_concat(std::move(cont))),
    }));
  }

  // The head of an `if` / `while` / `match` carries no brackets of its own, so
  // a break inside it lands between two operands (`if v ==` / `0 {`) and reads
  // as a mistake. It renders flat — except at the joints of a top-level
  // `||` / `&&` / `??` chain, which read as one test continued, and are all
  // that stands between a long condition and a line that runs off the page.
  // The whole condition sits one level deeper than the body that follows it,
  // so no continuation line can be mistaken for the first statement:
  //   if a ||
  //       b {
  //     body
  //   }
  DocP print_condition(const peg::Ast& node) {
    if (!is_binary_implicit(node.name)) return doc_flatten(print(node));
    return doc_indent(kIndent, print_binary(node, /*chain_only=*/true));
  }

  DocP print_range(const peg::Ast& node) {
    // `a..b`, `a..=b`, `a..`, `..b`, `..` — no spaces around the operator; the
    // optional step tail keeps its spaces (`0..n by 2`, never `0..nby 2`).
    std::vector<DocP> parts;
    for (auto& c : node.nodes) {
      if (c->name == "RANGE_OPERATOR") parts.push_back(doc_text(std::string(c->token)));
      else if (c->name == "BY_STEP")
        parts.push_back(doc_concat({
            doc_text(" by "),
            print_operand(*c->nodes[0], prec("RANGE"), /*assoc_safe=*/true),
        }));
      else parts.push_back(print_operand(*c, prec("RANGE"), /*assoc_safe=*/true));
    }
    return doc_concat(std::move(parts));
  }

  DocP print_unary(const peg::Ast& node) {
    // [OPERATOR_token, operand]
    std::string op = std::string(node.nodes[0]->token);
    // The operand needs parens only when it binds looser than unary itself, so
    // `-(a + b)` keeps them while `-a * b` and `-a ** b` — which bind tighter —
    // do not gain any. A stacked unary is the exception: `--x` and `!!x` lex as
    // one token each, so `-(-x)` has to stay parenthesized to round-trip.
    const peg::Ast& inner = *node.nodes[1];
    const bool stacked = inner.name.rfind("UNARY_", 0) == 0;
    DocP operand = stacked
                       ? doc_concat({doc_text("("), print(inner), doc_text(")")})
                       : print_operand(inner, /*parent_prec=*/prec(node.name),
                                       /*assoc_safe=*/true);
    return doc_concat({doc_text(op), operand});
  }

  DocP print_ternary(const peg::Ast& node) {
    // [cond, then, else]
    DocP cond = print_operand(*node.nodes[0], prec("CONDITIONAL"), /*assoc_safe=*/true);
    DocP then = print(*node.nodes[1]);
    DocP els = print_operand(*node.nodes[2], prec("CONDITIONAL"), /*assoc_safe=*/true);
    // A branch that breaks on its own wraps nothing useful, exactly as in a
    // binary chain: stay on one line and let the branch break inside itself.
    if (doc_has_hardline(cond) || doc_has_hardline(then) || doc_has_hardline(els))
      return doc_concat({cond, doc_text(" ? "), std::move(then), doc_text(" : "),
                         std::move(els)});
    // Wrap like a binary chain when it overflows: `?` and `:` lead the
    // continuation lines, which parses because both are infix rungs that admit
    // a newline before them (unlike `+`, where breaking early ends the term).
    return doc_group(doc_concat({
        cond,
        doc_indent(kIndent, doc_concat({
            doc_line(), doc_text("? "), std::move(then),
            doc_line(), doc_text(": "), std::move(els),
        })),
    }));
  }

  // A comma-separated, group-wrappable list: `( a, b, c )` style. `open`/`close`
  // are the delimiters; each element is already a Doc.
  DocP print_delimited(const std::string& open, std::vector<DocP> items,
                       const std::string& close) {
    if (items.empty()) return doc_text(open + close);
    // Breaking after the delimiter never helps a lone element: an atom is just
    // as wide on its own line, and a nested list breaks inside itself. Keep it
    // against the bracket (prettier's "hug") so `f({...})` reads as one call.
    if (items.size() == 1)
      return doc_concat({doc_text(open), std::move(items[0]), doc_text(close)});
    // Elements that bring their own line breaks: a function body, a block
    // expression, a literal holding a comment.
    int blocks = 0;
    size_t block = 0;
    for (size_t i = 0; i < items.size(); i++)
      if (doc_has_hardline(items[i])) { blocks++; block = i; }

    // One element breaks and at most one follows it: the list is multi-line no
    // matter what, so splitting it only buys a level of indentation. Keep it
    // hugged, the way `srv.get("/path", fn (req) { ... })` and
    // `assert_throws(fn () { ... }, "TypeError")` are written. Allowing one
    // element after the block is what keeps a trailing option (`workers: 4`,
    // `frames: 36000`) from costing the whole list its shape — dart's formatter
    // draws the line in the same place, for the same reason.
    if (blocks == 1 && block + 2 >= items.size()) {
      std::vector<DocP> hug{doc_text(open)};
      for (size_t i = 0; i < items.size(); i++) {
        if (i) hug.push_back(doc_text(", "));
        hug.push_back(std::move(items[i]));
      }
      hug.push_back(doc_text(close));
      return doc_concat(std::move(hug));
    }

    // Any other list with a break inside it goes one element per line. It can
    // never lay out flat, so it is built from hardlines rather than wrapped in
    // a Group: doc_fits reports "fits" the moment it reaches a hardline, and
    // everything past that point would then render as if flat. Always broken,
    // so the trailing comma is unconditional.
    if (blocks > 0) {
      std::vector<DocP> broken;
      for (auto& item : items) {
        broken.push_back(doc_hardline());
        broken.push_back(std::move(item));
        broken.push_back(doc_text(","));
      }
      return doc_concat({
          doc_text(open),
          doc_indent(kIndent, doc_concat(std::move(broken))),
          doc_hardline(),
          doc_text(close),
      });
    }

    std::vector<DocP> inner;
    inner.push_back(doc_softline());
    for (size_t i = 0; i < items.size(); i++) {
      inner.push_back(items[i]);
      if (i + 1 < items.size()) {
        inner.push_back(doc_text(","));
        inner.push_back(doc_line());
      }
    }
    // Once broken, the list gets the trailing comma the language allows for
    // exactly this reason: reordering or appending touches one line, not two.
    inner.push_back(doc_if_break(","));
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

    std::vector<DocP> segs{receiver};
    for (size_t i = 1; i < node.nodes.size(); i++)
      segs.push_back(print_postfix(*node.nodes[i]));

    // A chain carrying a block argument has nothing worth wrapping either: the
    // indent below would push that argument list a level deeper than the same
    // call written without the chain. Such a chain stays flat.
    if (calls < 2 || std::any_of(segs.begin(), segs.end(), doc_has_hardline))
      return doc_concat(std::move(segs));

    std::vector<DocP> cont;
    for (size_t i = 1; i < node.nodes.size(); i++) {
      if (is_dot(*node.nodes[i])) cont.push_back(doc_softline());  // break before `.`
      cont.push_back(std::move(segs[i]));
    }
    return doc_group(doc_concat({receiver, doc_indent(kIndent, doc_concat(std::move(cont)))}));
  }

  // An lvalue chain held in `node`'s children [off, off + cnt): the head
  // target followed by its postfix segments. Shared by ASSIGNMENT and
  // PLACE_ASSIGN, whose targets have exactly this shape.
  DocP print_lvalue_chain(const peg::Ast& node, size_t off, int cnt) {
    std::vector<DocP> parts{print(*node.nodes[off])};
    for (int k = 1; k < cnt; k++)
      parts.push_back(print_postfix(*node.nodes[off + k]));
    return doc_concat(std::move(parts));
  }

  DocP print_assignment(const peg::Ast& node) {
    auto v = view_assignment(node);
    std::vector<DocP> parts;
    if (v.is_let) parts.push_back(doc_text("let "));
    if (v.is_mut) parts.push_back(doc_text("mut "));
    parts.push_back(print_lvalue_chain(node, v.lvaloff, v.lvalcnt));
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

  // True when a comment sits anywhere inside `node`'s brace pair — its own
  // comments and those of anything nested within it.
  //
  // Unlike attachment, this is a layout question, so it asks about containment
  // rather than ownership. A line comment cannot share a line with what follows
  // it, so a literal holding one anywhere inside must lay out broken — and
  // since a Doc that hard-breaks may not sit inside a group, every level above
  // it inside the same literal has to break along with it.
  bool brace_holds_comment(const peg::Ast& node) const {
    BlockSpan span = block_interior(node.position);
    if (!span.found) return false;
    for (const auto& c : comments_) {
      if (c.start >= span.after) break;  // comments_ is sorted by start
      if (c.start >= span.lo) return true;
    }
    return false;
  }

  // A brace literal holding comments lays out like a block: one element per
  // line, its comments placed by the same owner test a block's statements get.
  // The trailing `,` is what the broken form already prints and what a
  // 1-element Set requires.
  //
  // `node.position` is the literal's `{` unless the optimizer folded a lone-
  // statement block onto it — a literal alone in a block wears that block's
  // braces. Scanning from there would hand the literal the BLOCK's interior,
  // so its elements and the block's statements would both print the comments
  // that interior owns. Shed the swallowed layer; the next `{` is the
  // literal's own — the same fold print_defer / print_try / print_cond dodge.
  // The collapse check is needed here and not in print_cond: uncollapsed, a
  // brace literal's position is already its own `{`, while a cond's is `cond`.
  template <typename Render>
  DocP print_brace_literal(const peg::Ast& node, Render render) {
    std::vector<const peg::Ast*> elems;
    for (auto& e : node.nodes) elems.push_back(e.get());
    size_t from = node.position;
    if (is_wrapper_collapsed(node) && from < src_.size() && is_code_[from] &&
        src_[from] == '{')
      from++;
    return print_braced(elems, from, ",", /*empty_ok=*/true, render);
  }

  DocP print_set(const peg::Ast& node) {
    if (brace_holds_comment(node))
      return print_brace_literal(node, [&](size_t k) { return print(*node.nodes[k]); });
    std::vector<DocP> items;
    for (auto& e : node.nodes) items.push_back(print(*e));
    // A 1-element Set must keep its trailing comma (`{a,}`) for the same
    // reason a 1-tuple must: without it the braces read as an Object literal,
    // and `{a}` does not parse at all. print_delimited would drop it.
    if (items.size() == 1)
      return doc_concat({doc_text("{"), items[0], doc_text(",}")});
    return print_delimited("{", std::move(items), "}");
  }

  // A 1-tuple must keep its trailing comma (`(a,)`); print_delimited would
  // drop it and turn the tuple into a plain parenthesized expression. Shared
  // with PLACE_ASSIGN's target list, which is parenthesized the same way.
  DocP print_tuple_items(std::vector<DocP> items) {
    if (items.size() == 1)
      return doc_concat({doc_text("("), items[0], doc_text(",)")});
    return print_delimited("(", std::move(items), ")");
  }

  DocP print_tuple(const peg::Ast& node) {
    std::vector<DocP> items;
    for (auto& e : node.nodes) items.push_back(print(*e));
    return print_tuple_items(std::move(items));
  }

  DocP print_object_property(const peg::Ast& p) {
    if (p.original_name == "SPREAD_ELEM")
      return doc_concat({doc_text("..."), print(*p.nodes[0])});
    auto ov = view_object_property(p);
    // Non-identifier literal keys slice verbatim; identifiers use the token.
    DocP keyd = (ov.key->name == "IDENTIFIER") ? doc_text(std::string(ov.key->token))
                                               : verbatim(*ov.key);
    DocP muts = ov.is_mut ? doc_text("mut ") : doc_text("");
    if (ov.is_shorthand) return doc_concat({muts, keyd});
    return doc_concat({muts, keyd, doc_text(": "), print(*ov.value)});
  }

  DocP print_object(const peg::Ast& node) {
    if (brace_holds_comment(node))
      return print_brace_literal(
          node, [&](size_t k) { return print_object_property(*node.nodes[k]); });
    if (node.nodes.empty()) return doc_text("{}");
    std::vector<DocP> items;
    for (auto& p : node.nodes) items.push_back(print_object_property(*p));
    return print_delimited("{", std::move(items), "}");
  }

  // A destructuring pattern: containers (`(...)` / `[...]` / `{...}`) get
  // normalized comma spacing; leaf patterns (identifier, `_`, `name: Type`,
  // `Ctor(a)`, literals, `a | b` alternation) are sliced verbatim.
  // A multi-target `for` binds bare: `for k, v in obj`, which is how the docs
  // and the examples write it. Only FOR_BINDING qualifies — `for (a, b) in
  // pairs` is a *tuple pattern* destructuring each element, a different
  // construct, and dropping its parentheses would change what the loop means.
  DocP print_for_binding(const peg::Ast& b) {
    if (b.name != "FOR_BINDING") return print_pattern(b);
    std::vector<DocP> parts;
    for (size_t i = 0; i < b.nodes.size(); i++) {
      if (i) parts.push_back(doc_text(", "));
      parts.push_back(print_pattern(*b.nodes[i]));
    }
    return doc_concat(std::move(parts));
  }

  DocP print_pattern(const peg::Ast& n) {
    // FOR_BINDING (multi-target `for k, v in …`) shares the tuple pattern's
    // shape; parenthesize it here — print_for_binding strips them back off for
    // the one context that writes them bare.
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

  // [target..., EXPRESSION]. A target is an lvalue chain (PLACE) or, with no
  // postfix, the bare IDENTIFIER it collapsed to. Chains print through the
  // same helper as ASSIGNMENT's lvalue, so `p[ 0 ]` normalizes the way
  // `p[ 0 ] = v` already does.
  DocP print_place_assign(const peg::Ast& node) {
    using namespace peg::udl;
    auto pv = view_place_assign(node);
    std::vector<DocP> targets;
    for (size_t i = 0; i < pv.count; i++) {
      const auto& t = *node.nodes[i];
      targets.push_back(
          t.tag == "PLACE"_
              ? print_lvalue_chain(t, 0, static_cast<int>(t.nodes.size()))
              : print(t));
    }
    return doc_concat({print_tuple_items(std::move(targets)), doc_text(" = "),
                       print(*pv.rhs)});
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
    parts.push_back(print_condition(*nodes[off]));
    parts.push_back(doc_text(" "));
    parts.push_back(print_block(*nodes[off + 1], node_end(*nodes[off]), false));
    size_t cursor = block_interior(node_end(*nodes[off])).after;
    size_t i = off + 2, n = nodes.size();
    while (n - i >= 2) {
      parts.push_back(doc_text(" else if "));
      parts.push_back(print_condition(*nodes[i]));
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
    DocP head = print_condition(*wv.cond);
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
    return doc_concat({doc_text("for "), print_for_binding(*fv.binding),
                       doc_text(" in "), doc_flatten(print(*fv.iter)), doc_text(" "),
                       print_block(*fv.body, node_end(*fv.iter), false),
                       print_nobreak(fv.nobreak, *fv.body)});
  }

  // print_defer / print_try: the `{` follows the keyword directly, so the scan
  // starts at the body's own position rather than the statement's. A lone
  // `defer` / `try` inherits its block's span (the optimizer's single-child
  // widening), and `node.position` would then find the ENCLOSING brace,
  // handing the body comments the block already prints. print_lexical_scope
  // shakes off the same fold, and print_cond sheds it a third way.
  DocP print_defer(const peg::Ast& node) {
    return doc_concat({doc_text("defer "),
                       print_block(*node.nodes[0], node.nodes[0]->position,
                                   true)});
  }

  DocP print_try(const peg::Ast& node) {
    // [BLOCK, IDENTIFIER, BLOCK]. The try block's `{` follows `try`; the catch
    // block's `{` follows the bound identifier.
    return doc_concat({doc_text("try "),
                       print_block(*node.nodes[0], node.nodes[0]->position,
                                   false),
                       doc_text(" catch " + std::string(node.nodes[1]->token) + " "),
                       print_block(*node.nodes[2], node_end(*node.nodes[1]), false)});
  }

  DocP print_keyword_expr(const std::string& kw, const peg::Ast& node) {
    // RETURN / THROW / YIELD: keyword with optional trailing expression.
    if (node.nodes.empty()) return doc_text(kw);
    return doc_concat({doc_text(kw + " "), print(*node.nodes[0])});
  }

  // `stmt if cond` / `stmt unless cond` — parse_for_format keeps this shape
  // intact (see view_postfix_modifier), so it prints back as the one
  // canonical one-line form rather than the desugared multi-line `if cond {
  // stmt }` the interp/JIT path would otherwise see.
  DocP print_postfix_modifier(const peg::Ast& node) {
    auto pv = culebra::view_postfix_modifier(node);
    const char* kw = pv.is_unless ? " unless " : " if ";
    return doc_concat({print(*pv.base), doc_text(kw), print(*pv.cond)});
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
    head.push_back(print_condition(*mv.subject));
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
    // `cond` is the third keyword whose `{` follows it directly, so a lone one
    // inherits its block's span and `node.position` is the ENCLOSING brace.
    // There is no body node to scan from here — the arms start inside the
    // brace — so shed the swallowed layer instead: a span that opens on a
    // code `{` is the fold, and the next `{` past it is the cond's own.
    size_t from = node.position;
    if (from < src_.size() && is_code_[from] && src_[from] == '{') from++;
    return doc_concat({doc_text("cond "),
                       print_braced(items, from, ",", /*empty_ok=*/true, render)});
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
    if (v.is_field)  // `static x = expr` / untyped instance field `x = expr`
      return doc_concat(
          {doc_text((v.is_static ? "static " : "") + std::string(v.name) +
                    " = "),
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
    if (n == "PLACE_ASSIGN") return print_place_assign(node);
    if (n == "IMPORT_STMT") return print_import(node);
    if (n == "EXPORT_STMT") return print_export(node);
    if (n == "RETURN") return print_keyword_expr("return", node);
    if (n == "THROW") return print_keyword_expr("throw", node);
    if (n == "YIELD") return print_keyword_expr("yield", node);
    if (n == "STATEMENT") return print_postfix_modifier(node);

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

// Line up the trailing comments of adjacent lines. A run of `code  # note`
// lines is written as a column by hand — giving each one its own width turns a
// table back into ragged text. Only runs at the same indentation are joined, so
// a comment inside a block never drags one outside it. Two spaces is the
// minimum gap, which is what a single such line already gets.
inline std::string align_trailing_comments(const std::string& src) {
  std::vector<size_t> line_start{0};
  for (size_t i = 0; i < src.size(); i++)
    if (src[i] == '\n') line_start.push_back(i + 1);
  const size_t n = line_start.size();

  std::vector<size_t> comment_at(n, std::string::npos);
  for (const auto& c : scan_comments(src)) {
    if (c.own_line || c.block) continue;
    size_t li = static_cast<size_t>(
        std::upper_bound(line_start.begin(), line_start.end(), c.start) -
        line_start.begin()) - 1;
    comment_at[li] = c.start;
  }

  auto line_end = [&](size_t li) { return li + 1 < n ? line_start[li + 1] : src.size(); };
  auto indent_of = [&](size_t li) {
    size_t p = line_start[li];
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) p++;
    return p - line_start[li];
  };
  auto code_width = [&](size_t li) {
    size_t e = comment_at[li];
    while (e > line_start[li] && (src[e - 1] == ' ' || src[e - 1] == '\t')) e--;
    return e - line_start[li];
  };

  std::string out;
  out.reserve(src.size() + src.size() / 16);
  for (size_t i = 0; i < n;) {
    if (comment_at[i] == std::string::npos) {
      out.append(src, line_start[i], line_end(i) - line_start[i]);
      i++;
      continue;
    }
    size_t j = i, width = 0;
    const size_t indent = indent_of(i);
    while (j < n && comment_at[j] != std::string::npos && indent_of(j) == indent) {
      width = std::max(width, code_width(j));
      j++;
    }
    for (size_t k = i; k < j; k++) {
      size_t w = code_width(k);
      out.append(src, line_start[k], w);
      out.append(width - w + 2, ' ');
      out.append(src, comment_at[k], line_end(k) - comment_at[k]);
    }
    i = j;
  }
  return out;
}

// Multiset of comment texts (sorted), so two sources can be compared for
// comment preservation regardless of where the comments moved.
inline std::vector<std::string> comment_multiset(std::string_view s) {
  std::vector<std::string> out;
  for (const auto& c : scan_comments(s))
    out.emplace_back(s.substr(c.start, c.end - c.start));
  std::sort(out.begin(), out.end());
  return out;
}

inline FormatResult format_source(const std::string& path,
                                  const std::string& src,
                                  int width = 80) {
  std::vector<std::string> msgs;
  // The parse normalizes newlines in place and the AST's tokens point into the
  // buffer it edited, so format a copy and leave `src` as the file was
  // written: the verdict at the end compares against that, and a CRLF file is
  // not already formatted — `fmt -i` rewrites it with LF endings.
  std::string text = src;
  auto ast = parse_for_format(path, text, msgs);
  if (!ast) {
    std::string m;
    for (auto& s : msgs) m += s;
    return {FormatStatus::ParseError, "", m};
  }

  Printer printer(text);
  std::string out = doc_render(printer.print_program(*ast), width);
  if (out.empty() || out.back() != '\n') out += '\n';
  out = align_trailing_comments(out);

  // Safety net 1: re-parse and require structural AST equality, so a printer
  // bug can never silently change program meaning.
  std::vector<std::string> msgs2;
  auto ast2 = parse_for_format(path, out, msgs2);
  if (!ast2 || !ast_equal(*ast, *ast2)) {
    return {FormatStatus::Refused, "",
            "culebra fmt: internal check failed (formatted output would change "
            "program meaning); left unchanged"};
  }

  // Safety net 2: comments are absent from the AST, so AST equality can't catch
  // a dropped or duplicated comment. Require the comment multiset to match.
  if (comment_multiset(text) != comment_multiset(out)) {
    return {FormatStatus::Refused, "",
            "culebra fmt: internal check failed (a comment would be dropped or "
            "moved incorrectly); left unchanged"};
  }

  if (out == src) return {FormatStatus::Unchanged, out, ""};
  return {FormatStatus::Ok, out, ""};
}

}  // namespace culebra::fmt
