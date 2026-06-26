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

inline bool scan_has_comment(std::string_view s) {
  // Stack of string contexts we are inside. Code (including interpolation
  // holes) is represented by the stack being empty or the top being Hole.
  enum class Ctx { Code, SQ, BT, DQ, Triple };
  std::vector<Ctx> st;
  size_t i = 0, n = s.size();
  auto top = [&] { return st.empty() ? Ctx::Code : st.back(); };
  while (i < n) {
    Ctx c = top();
    char ch = s[i];
    if (c == Ctx::Code) {
      if (ch == '#') return true;
      if (ch == '/' && i + 1 < n && (s[i + 1] == '/' )) return true;
      if (ch == '/' && i + 1 < n && s[i + 1] == '*') return true;
      if (ch == '\'') { st.push_back(Ctx::SQ); i++; continue; }
      if (ch == '`') { st.push_back(Ctx::BT); i++; continue; }
      if (ch == '"') {
        if (i + 2 < n && s[i + 1] == '"' && s[i + 2] == '"') { st.push_back(Ctx::Triple); i += 3; continue; }
        st.push_back(Ctx::DQ); i++; continue;
      }
      if (ch == '}' && !st.empty()) {
        // Closing an interpolation hole: but only when there is a string
        // context below. A bare `}` in pure code (block close) has no string
        // below, so the stack is empty here and we simply skip it.
        // (When we entered a hole we pushed nothing; holes are tracked by the
        // string context staying on the stack — see DQ/Triple handling below.)
      }
      i++;
      continue;
    }
    if (c == Ctx::SQ) {
      if (ch == '\'') { st.pop_back(); }
      i++;
      continue;
    }
    if (c == Ctx::BT) {
      if (ch == '`') { st.pop_back(); }
      i++;
      continue;
    }
    // DQ / Triple: handle escapes and interpolation holes.
    if (ch == '\\') { i += 2; continue; }
    if (ch == '{') {
      // Enter an interpolation hole: scan its body as code until the matching
      // '}'. Recurse via the stack by pushing Code and tracking brace depth.
      int depth = 1;
      i++;
      while (i < n && depth > 0) {
        char d = s[i];
        if (d == '#') return true;
        if (d == '/' && i + 1 < n && s[i + 1] == '/') return true;
        if (d == '/' && i + 1 < n && s[i + 1] == '*') return true;
        if (d == '{') depth++;
        else if (d == '}') depth--;
        else if (d == '\'') { // nested string in hole
          i++; while (i < n && s[i] != '\'') i++;
        } else if (d == '`') {
          i++; while (i < n && s[i] != '`') i++;
        } else if (d == '"') {
          i++; while (i < n && s[i] != '"') { if (s[i] == '\\') i++; i++; }
        }
        i++;
      }
      continue;
    }
    if (c == Ctx::DQ && ch == '"') { st.pop_back(); i++; continue; }
    if (c == Ctx::Triple && ch == '"' && i + 2 < n && s[i + 1] == '"' && s[i + 2] == '"') {
      st.pop_back(); i += 3; continue;
    }
    i++;
  }
  return false;
}

// ----------------------------------------------------------------------------
// AST structural equality (safety net)
// ----------------------------------------------------------------------------

inline bool ast_equal(const peg::Ast& a, const peg::Ast& b) {
  if (a.name != b.name) return false;
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
  explicit Printer(std::string_view src) : src_(src) {}

  DocP print_program(const peg::Ast& program) {
    // PROGRAM collapses to STATEMENTS; if there is a single statement it
    // collapses to that statement. Normalize to a child list.
    return print_statement_list(stmt_children(program), /*top=*/true);
  }

 private:
  std::string_view src_;
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

  // Count blank lines between two adjacent statements in the source so we can
  // preserve a single intentional blank (collapsing runs of >=2 to one).
  bool blank_between(const peg::Ast& a, const peg::Ast& b) const {
    size_t end = a.position + a.length;
    size_t start = b.position;
    if (start <= end || start > src_.size()) return false;
    int newlines = 0;
    for (size_t i = end; i < start; i++)
      if (src_[i] == '\n') newlines++;
    return newlines >= 2;
  }

  DocP print_statement_list(const std::vector<const peg::Ast*>& stmts, bool /*top*/) {
    std::vector<DocP> parts;
    for (size_t i = 0; i < stmts.size(); i++) {
      if (i > 0) {
        parts.push_back(doc_hardline());
        if (blank_between(*stmts[i - 1], *stmts[i])) parts.push_back(doc_hardline());
      }
      parts.push_back(print(*stmts[i]));
    }
    return doc_concat(std::move(parts));
  }

  // A brace block: always multi-line. The node is the block body (either a
  // STATEMENTS node, a single collapsed statement, or empty).
  DocP print_block(const peg::Ast& body, bool empty_ok) {
    auto stmts = stmt_children(body);
    if (stmts.empty() && empty_ok) return doc_text("{}");
    std::vector<DocP> inner;
    inner.push_back(doc_text("{"));
    inner.push_back(doc_indent(kIndent,
        doc_concat({doc_hardline(), print_statement_list(stmts, false)})));
    inner.push_back(doc_hardline());
    inner.push_back(doc_text("}"));
    return doc_concat(std::move(inner));
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
    std::vector<DocP> parts;
    for (size_t k = 0; k < m; k++) {
      bool assoc_safe = right_assoc ? (k == m - 1) : (k == 0);
      parts.push_back(print_operand(*operands[k], P, assoc_safe));
      if (k + 1 < m) {
        parts.push_back(doc_text(" " + ops[k] + " "));
      }
    }
    return doc_concat(std::move(parts));
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
    std::vector<DocP> parts;
    // The receiver must keep parens when it binds looser than a postfix op, so
    // `(-3).double()` / `(a + b).x` don't collapse to `-3.double()` (which
    // parses as `-(3.double())`) or `a + b.x`.
    parts.push_back(print_operand(*node.nodes[0], /*parent_prec=*/prec("CALL"),
                                  /*assoc_safe=*/true));
    for (size_t i = 1; i < node.nodes.size(); i++)
      parts.push_back(print_postfix(*node.nodes[i]));
    return doc_concat(std::move(parts));
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

  DocP print_params(const peg::Ast& params) {
    std::vector<DocP> items;
    for (auto& p : params.nodes) items.push_back(verbatim(*p));  // params: verbatim
    return print_delimited("(", std::move(items), ")");
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
    parts.push_back(print_block(*node.nodes[i], /*empty_ok=*/true));
    return doc_concat(std::move(parts));
  }

  // RETURN_TYPE / TYPE_ANNOTATION carry their captured type token; emit it.
  std::string slice_inner(const peg::Ast& a) const {
    return std::string(a.token.empty() ? slice(a) : std::string(a.token));
  }

  DocP print_lambda(const peg::Ast& node) {
    // [LAMBDA_PARAMS, EXPRESSION]
    std::vector<DocP> items;
    for (auto& p : node.nodes[0]->nodes) items.push_back(verbatim(*p));
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
    // [cond, then, (cond, block)*, (block)?]
    std::vector<DocP> parts;
    parts.push_back(doc_text("if "));
    parts.push_back(print(*node.nodes[0]));
    parts.push_back(doc_text(" "));
    parts.push_back(print_block(*node.nodes[1], /*empty_ok=*/false));
    size_t i = 2, n = node.nodes.size();
    while (n - i >= 2) {
      parts.push_back(doc_text(" else if "));
      parts.push_back(print(*node.nodes[i]));
      parts.push_back(doc_text(" "));
      parts.push_back(print_block(*node.nodes[i + 1], false));
      i += 2;
    }
    if (i < n) {
      parts.push_back(doc_text(" else "));
      parts.push_back(print_block(*node.nodes[i], false));
    }
    return doc_concat(std::move(parts));
  }

  DocP print_while(const peg::Ast& node) {
    return doc_concat({doc_text("while "), print(*node.nodes[0]), doc_text(" "),
                       print_block(*node.nodes[1], false)});
  }

  DocP print_for(const peg::Ast& node) {
    // [binding, iter, BLOCK]. Binding slices verbatim (single var or pattern).
    return doc_concat({doc_text("for "), verbatim(*node.nodes[0]), doc_text(" in "),
                       print(*node.nodes[1]), doc_text(" "),
                       print_block(*node.nodes[2], false)});
  }

  DocP print_defer(const peg::Ast& node) {
    return doc_concat({doc_text("defer "), print_block(*node.nodes[0], true)});
  }

  DocP print_try(const peg::Ast& node) {
    // [BLOCK, IDENTIFIER, BLOCK]
    return doc_concat({doc_text("try "), print_block(*node.nodes[0], false),
                       doc_text(" catch " + std::string(node.nodes[1]->token) + " "),
                       print_block(*node.nodes[2], false)});
  }

  DocP print_keyword_expr(const std::string& kw, const peg::Ast& node) {
    // RETURN / THROW / YIELD: keyword with optional trailing expression.
    if (node.nodes.empty()) return doc_text(kw);
    return doc_concat({doc_text(kw + " "), print(*node.nodes[0])});
  }

 public:
  DocP print(const peg::Ast& node) {
    const std::string& n = node.name;

    // Statements / control flow
    if (n == "STATEMENTS") return print_block(node, false);
    if (n == "ASSIGNMENT") return print_assignment(node);
    if (n == "MULTIFN_DECL") return print_function(node, /*named=*/true);
    if (n == "FUNCTION") return print_function(node, /*named=*/false);
    if (n == "LAMBDA") return print_lambda(node);
    if (n == "IF") return print_if(node);
    if (n == "WHILE") return print_while(node);
    if (n == "FOR") return print_for(node);
    if (n == "DEFER") return print_defer(node);
    if (n == "TRY") return print_try(node);
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

enum class FormatStatus { Ok, Unchanged, SkippedComments, ParseError, Refused };

struct FormatResult {
  FormatStatus status;
  std::string output;   // valid when Ok / Unchanged
  std::string message;  // diagnostic for ParseError / Refused
};

// `drop_comments` is a test-only escape hatch: it bypasses the Phase 0 comment
// guard so the printer + safety net can be exercised over comment-bearing
// source (the comments are LOST — never use this for real formatting). It lets
// the test harness measure printer robustness across the whole corpus before
// comment preservation (Phase 1) lands.
inline FormatResult format_source(const std::string& path, std::string_view src,
                                  int width = 80, bool drop_comments = false) {
  // Phase 0 cannot preserve comments (the grammar drops them), so refuse to
  // touch files that contain any — leaving them byte-for-byte unchanged rather
  // than silently deleting comments.
  if (!drop_comments && scan_has_comment(src))
    return {FormatStatus::SkippedComments, std::string(src), ""};

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

  // Safety: re-parse and require structural AST equality.
  std::vector<std::string> msgs2;
  auto ast2 = parse_for_format(path, out.data(), out.size(), msgs2);
  if (!ast2 || !ast_equal(*ast, *ast2)) {
    return {FormatStatus::Refused,
            "",
            "culebra fmt: internal check failed (formatted output would change "
            "program meaning); left unchanged"};
  }

  if (out == src) return {FormatStatus::Unchanged, out, ""};
  return {FormatStatus::Ok, out, ""};
}

}  // namespace culebra::fmt
