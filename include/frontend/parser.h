#pragma once

#include <peglib.h>

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <format>
#include <optional>
#include <print>
#include <set>
#include <string>
#include <string_view>

#include "base/shared.h"
#include <vector>

#include "frontend/grammar_def.h"       // culebra::grammar_ — single source of truth
#include "frontend/grammar_blob.gen.h"      // prebuilt serialized grammar (generated; `just gen-blob`)
#include "frontend/grammar_blob_key.h"  // grammar_blob_key() — shared with the blob generator

namespace culebra {

// Nesting-depth guard for the PEG's recursive descent. Machine-written
// nesting (20k `(`s or `[`s) otherwise overflows the C stack inside
// peglib — an uncatchable SIGSEGV instead of a SyntaxError. The unit is
// *rule* entries, not brackets: one literal nesting level costs ~6 rules
// (measured), the deepest file in the test corpus reaches 101, and the
// crash needs ~120k — so 4000 allows ~650-deep generated literals with
// 40x corpus headroom while staying two orders under the cliff, which
// also keeps every downstream AST walker (optimizer, transforms, both
// backends' compilers, the formatter) far from its own stack budget.
// Reset per parse: a throw from the enter hook aborts the parse before
// peglib's scope_exit can rebalance the count.
inline constexpr int64_t kCulebraParseDepthLimit = 4000;
inline thread_local int64_t _culebra_parse_depth = 0;

inline peg::parser& get_parser() {
  // thread_local — peg::parser's logger callback and VM state aren't
  // safe to share across host threads.
  static thread_local peg::parser parser;
  static thread_local bool initialized = false;

  if (!initialized) {
    initialized = true;

    parser.set_logger([&](size_t ln, size_t col, const std::string& msg) {
      std::println(stderr, "{}:{}: {}", ln, col, msg);
    });

    // Fast path: load the prebuilt blob, skipping peglib's ~10 ms meta-parse.
    // The hash guard falls back to load_grammar() if the blob is stale (grammar
    // edited without `just gen-blob`, or a peglib bump changed the layout).
    // Both paths then enable AST + packrat identically — load_blob() restores
    // the parser-level packrat flag from the blob (cpp-peglib >= 9b63764), so
    // enable_packrat_parsing() re-applies it rather than resetting it.
    bool loaded = false;
    if (GRAMMAR_BLOB_HASH == grammar_blob_key()) {
      std::vector<uint8_t> blob(GRAMMAR_BLOB, GRAMMAR_BLOB + GRAMMAR_BLOB_SIZE);
      loaded = parser.load_blob(blob);
    }
    if (!loaded && !parser.load_grammar(grammar_)) {
      throw std::logic_error("invalid peg grammar");
    }

    parser.enable_ast();
    parser.enable_packrat_parsing();

    // Every nesting construct passes through EXPRESSION or STATEMENT, so
    // two hooks bound the whole grammar's recursion. peglib runs `leave`
    // from a scope_exit, so backtracking keeps the count balanced.
    auto enter_depth = [](const peg::Context& c, const char* s, size_t,
                          std::any&) {
      if (++_culebra_parse_depth > kCulebraParseDepthLimit) {
        auto [ln, col] = c.line_info(s);
        throw CulebraError(
            "SyntaxError", nesting_too_deep_message(kCulebraParseDepthLimit),
            static_cast<long>(ln), static_cast<long>(col));
      }
    };
    auto leave_depth = [](const peg::Context&, const char*, size_t, size_t,
                          std::any&, std::any&) { --_culebra_parse_depth; };
    // Every named rule counts: deep nesting descends through whichever
    // recursive rule family matches first (a 20k-`[` tower dives through
    // the *pattern* rules while probing DESTRUCTURE_ASSIGN, never touching
    // EXPRESSION), so hooking a hand-picked subset is a losing game.
    for (const auto& [rule_name, def] : parser.get_grammar()) {
      parser[rule_name.c_str()].enter = enter_depth;
      parser[rule_name.c_str()].leave = leave_depth;
    }
  }

  return parser;
}

// Source newlines are LF; a CRLF file is accepted and normalized here, in
// place, before the grammar sees a byte of it. Left alone, the `\r` lands
// inside triple-quoted and raw string literals — a value difference, not a
// formatting one, which is why a CRLF checkout failed four tests on Windows
// and nowhere else. The buffer is edited rather than copied because the AST's
// tokens are string_views into it.
//
// A lone `\r` is rejected instead of guessed at: it is either an old-Mac line
// ending or a stray byte, and silently choosing one is the kind of run-time
// rescue culebra refuses elsewhere (Rust rejects it too; Go and Python drop
// it). Normalizing is idempotent, so a caller that parses the same buffer
// twice — `culebra lint` does — gets the same bytes both times.
inline void normalize_source_newlines(std::string& src) {
  auto first = src.find('\r');
  if (first == std::string::npos) return;

  int64_t line = 1, col = 1;
  for (size_t i = 0; i < first; i++) {
    if (src[i] == '\n') { line++; col = 1; } else { col++; }
  }
  size_t w = first;
  for (size_t r = first; r < src.size(); r++) {
    char c = src[r];
    if (c == '\r') {
      if (r + 1 >= src.size() || src[r + 1] != '\n') {
        throw CulebraError("SyntaxError",
            "a bare carriage return is not a line ending: source newlines are "
            "LF or CRLF.", line, col);
      }
      col++;
      continue;  // drop it — the \n right after is the line ending
    }
    src[w++] = c;
    if (c == '\n') { line++; col = 1; } else { col++; }
  }
  src.resize(w);
}

// `\xHH` — exactly two hex digits → one raw byte (0x00–0xFF). `i` points
// at the backslash; returns the index of the last consumed char.
inline size_t decode_hex_byte(std::string_view raw, size_t i, std::string& out) {
  if (i + 3 >= raw.size() ||
      hex_digit(raw[i + 2]) < 0 || hex_digit(raw[i + 3]) < 0) {
    throw CulebraError("SyntaxError",
        "invalid \\x escape: expected two hex digits (\\xHH).");
  }
  out += static_cast<char>(hex_digit(raw[i + 2]) * 16 + hex_digit(raw[i + 3]));
  return i + 3;
}

// Parses exactly `width` hex digits starting at raw[start]. `escape_name` is
// the introducer (e.g. "\\u") used in error messages.
inline uint32_t parse_fixed_hex(std::string_view raw, size_t start, int width,
                                 std::string_view escape_name) {
  if (start + width > raw.size()) {
    throw CulebraError("SyntaxError", std::format(
        "invalid {} escape: expected {} hex digits.", escape_name, width));
  }
  uint32_t cp = 0;
  for (int k = 0; k < width; k++) {
    int v = hex_digit(raw[start + k]);
    if (v < 0) {
      throw CulebraError("SyntaxError", std::format(
          "invalid {} escape: expected {} hex digits.", escape_name, width));
    }
    cp = cp * 16 + static_cast<uint32_t>(v);
  }
  return cp;
}

// `\uXXXX` — exactly 4 hex digits → a BMP Unicode scalar value, UTF-8
// encoded. Rejects surrogates (U+D800–U+DFFF). `i` points at the backslash;
// returns the index of the last consumed hex digit.
inline size_t decode_unicode_escape_u(std::string_view raw, size_t i,
                                       std::string& out) {
  uint32_t cp = parse_fixed_hex(raw, i + 2, 4, "\\u");
  if (cp >= 0xD800 && cp <= 0xDFFF) {
    throw CulebraError("SyntaxError", std::format(
        "invalid \\u escape: U+{:04X} is not a Unicode scalar value "
        "(surrogate).", cp));
  }
  append_utf8(out, cp);
  return i + 5;
}

// `\UXXXXXXXX` — exactly 8 hex digits → a Unicode scalar value, UTF-8
// encoded. Rejects > U+10FFFF and surrogates. `i` points at the backslash;
// returns the index of the last consumed hex digit.
inline size_t decode_unicode_escape_U(std::string_view raw, size_t i,
                                       std::string& out) {
  uint32_t cp = parse_fixed_hex(raw, i + 2, 8, "\\U");
  if (!is_unicode_scalar_value(cp)) {
    throw CulebraError("SyntaxError", std::format(
        "invalid \\U escape: U+{:X} is not a Unicode scalar value.", cp));
  }
  append_utf8(out, cp);
  return i + 9;
}

// Decode escape sequences in an INTERPOLATED_CONTENT token. Recognized:
//   \n \r \t \\ \" \{ \xHH \uXXXX \UXXXXXXXX
// Unknown '\X' is preserved literally as backslash + char (Python's
// permissive default — keeps the string round-trippable rather than
// silently dropping the escape introducer).
inline std::string decode_interpolated_content(std::string_view raw) {
  // Common case: most plain-text segments contain no backslash. Skip the
  // per-byte loop and emit the source bytes verbatim.
  if (raw.find('\\') == std::string_view::npos) {
    return std::string(raw);
  }
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); i++) {
    char c = raw[i];
    if (c == '\\' && i + 1 < raw.size()) {
      char n = raw[i + 1];
      switch (n) {
        case 'n':  out += '\n'; i++; continue;
        case 'r':  out += '\r'; i++; continue;
        case 't':  out += '\t'; i++; continue;
        case '\\': out += '\\'; i++; continue;
        case '"':  out += '"';  i++; continue;
        case '{':  out += '{';  i++; continue;
        case 'x':  i = decode_hex_byte(raw, i, out); continue;
        case 'u':  i = decode_unicode_escape_u(raw, i, out); continue;
        case 'U':  i = decode_unicode_escape_U(raw, i, out); continue;
        default:
          out += '\\';
          out += n;
          i++;
          continue;
      }
    }
    out += c;
  }
  return out;
}

// A normalized piece of an interpolated/triple string: either a literal text
// chunk (still escape-encoded — the caller runs decode_interpolated_content on
// it) or an INTERP_EXPR (or defensive bare-expression) node to evaluate.
struct InterpPiece {
  std::string text;                 // literal chunk; valid when expr == nullptr
  const peg::Ast* expr = nullptr;   // INTERP_EXPR / bare expr node, else nullptr
};

// View of an INTERP_EXPR AST node — see grammar:
//   INTERP_EXPR <- '{' _ EXPRESSION (_ ':' FORMAT_SPEC)? _ '}'
//   FORMAT_SPEC <- (SPEC_ARG / SPEC_TEXT)*
// Layout: [EXPRESSION, (FORMAT_SPEC)?]; FORMAT_SPEC's children are SPEC_TEXT
// (a literal chunk, in `token`) and SPEC_ARG (whose lone child is the
// EXPRESSION spliced in). Both evaluators (interp append_interp_expr, JIT
// emit_interp_fragment) read the layout through this view; `constant_spec`
// keeps a field-free spec on their fixed-spec path.
struct InterpExprView {
  const peg::Ast* value;         // the expression being formatted
  const peg::Ast* spec;          // FORMAT_SPEC node, or null when there is none
  bool constant_spec;            // spec is absent or has no SPEC_ARG child
  std::string_view spec_text;    // the whole spec, valid when constant_spec
};

// One piece of a FORMAT_SPEC: a literal chunk, or the field expression whose
// value is spliced in. Both backends walk `spec->nodes` through this, so the
// piece layout lives here with the rest of the interpolation shape.
struct SpecPiece {
  std::string_view text;           // literal chunk; valid when expr == nullptr
  const peg::Ast* expr = nullptr;  // SPEC_ARG's EXPRESSION, else nullptr
};

inline SpecPiece view_spec_piece(const peg::Ast& piece) {
  using namespace peg::udl;
  if (piece.tag == "SPEC_TEXT"_) return {piece.token, nullptr};
  return {{}, piece.nodes[0].get()};
}

inline InterpExprView view_interp_expr(const peg::Ast& node) {
  using namespace peg::udl;
  // Defensive: a bare expression node (no INTERP_EXPR wrapper) formats plain.
  if (node.tag != "INTERP_EXPR"_) return {&node, nullptr, true, {}};
  const peg::Ast* spec = node.nodes.size() > 1 ? node.nodes[1].get() : nullptr;
  if (!spec) return {node.nodes[0].get(), nullptr, true, {}};
  std::string_view text;
  for (const auto& piece : spec->nodes) {
    if (piece->tag != "SPEC_TEXT"_) {
      return {node.nodes[0].get(), spec, false, {}};
    }
    text = piece->token;  // a constant spec is one chunk, or none
  }
  return {node.nodes[0].get(), spec, true, text};
}

// Swift-style "block string" normalization for a TRIPLE_STRING node. In BLOCK
// form (opening `"""` followed by a newline) the surrounding newlines are
// dropped and every line is dedented by the closing `"""`'s indentation (the
// authority — an under-indented non-blank line is a SyntaxError); blank lines
// become empty. Any other triple string (content on the opening line, incl. the
// single-line form) is returned verbatim. Either way the returned pieces are the
// single source both backends iterate, so the dedent can't diverge.
inline std::vector<InterpPiece> normalize_triple_pieces(const peg::Ast& ast) {
  using namespace peg::udl;

  auto verbatim = [&] {
    std::vector<InterpPiece> pieces;
    pieces.reserve(ast.nodes.size());
    for (const auto& child : ast.nodes) {
      if (child->tag == "TRIPLE_CONTENT"_) {
        pieces.push_back({std::string(child->token), nullptr});
      } else {
        pieces.push_back({{}, child.get()});
      }
    }
    return pieces;
  };

  // Block form requires the first child to be a TRIPLE_CONTENT whose text is
  // horizontal whitespace up to a newline.
  if (ast.nodes.empty() || ast.nodes.front()->tag != "TRIPLE_CONTENT"_) {
    return verbatim();
  }
  std::string_view head = ast.nodes.front()->token;
  size_t nl = head.find('\n');
  if (nl == std::string_view::npos) return verbatim();
  for (size_t i = 0; i < nl; i++) {
    char c = head[i];
    if (c != ' ' && c != '\t' && c != '\r') return verbatim();
  }

  // Split children into source lines (newlines are dropped here and re-added
  // when the dedented pieces are emitted). A line is a list of pieces.
  std::vector<std::vector<InterpPiece>> lines(1);
  auto push_text = [&](std::string_view t) {
    if (!t.empty()) lines.back().push_back({std::string(t), nullptr});
  };
  for (const auto& child : ast.nodes) {
    if (child->tag != "TRIPLE_CONTENT"_) {
      lines.back().push_back({{}, child.get()});
      continue;
    }
    std::string_view raw = child->token;
    size_t start = 0;
    for (size_t i = 0; i < raw.size(); i++) {
      if (raw[i] != '\n') continue;
      size_t end = i;
      if (end > start && raw[end - 1] == '\r') end--;  // CRLF → LF
      push_text(raw.substr(start, end - start));
      lines.emplace_back();
      start = i + 1;
    }
    push_text(raw.substr(start));
  }

  // The closing line (whitespace before the closing `"""`) sets the indent.
  std::string indent;
  for (const auto& p : lines.back()) {
    bool ws_only = !p.expr;
    if (ws_only)
      for (char c : p.text)
        if (c != ' ' && c != '\t') { ws_only = false; break; }
    if (!ws_only) {
      throw CulebraError("SyntaxError",
          "closing \"\"\" of a multi-line string must be on its own line.");
    }
    indent += p.text;
  }
  lines.pop_back();              // drop closing-delimiter line
  lines.erase(lines.begin());   // drop opening newline (whitespace-only)

  std::vector<InterpPiece> out;
  for (size_t li = 0; li < lines.size(); li++) {
    if (li > 0) out.push_back({"\n", nullptr});  // re-insert line breaks
    auto& line = lines[li];

    bool blank = true;
    for (const auto& p : line) {
      if (p.expr) { blank = false; break; }
      for (char c : p.text)
        if (c != ' ' && c != '\t') { blank = false; break; }
      if (!blank) break;
    }
    if (blank) continue;  // normalize blank lines to empty

    if (!indent.empty()) {
      if (line.front().expr || line.front().text.size() < indent.size() ||
          line.front().text.compare(0, indent.size(), indent) != 0) {
        throw CulebraError("SyntaxError",
            "insufficient indentation in multi-line string literal — every "
            "line must be indented to at least the closing \"\"\".");
      }
      line.front().text.erase(0, indent.size());
    }
    for (auto& p : line) out.push_back(std::move(p));
  }
  return out;
}

// Extract an optional TYPE_ANNOTATION sibling (the captured type name) from
// an AST node, returning {} if none is present. The expected position is
// `node.nodes[index]`; pass the tentative slot to probe.
inline std::string_view extract_type_annotation(const peg::Ast& node,
                                                size_t index) {
  using namespace peg::udl;
  if (index < node.nodes.size() &&
      node.nodes[index]->tag == "TYPE_ANNOTATION"_) {
    return node.nodes[index]->token;
  }
  return {};
}

// View of an ASSIGNMENT AST node — see grammar:
//   ASSIGNMENT <- LET _ MUTABLE _ PRIMARY (_h_ (ARGUMENTS / INDEX) / _ DOT)*
//                 (_ TYPE_ANNOTATION)? _ ASSIGN_OP _ EXPRESSION
// Layout: [LET, MUTABLE, lval-chain..., (TYPE_ANNOTATION)?, ASSIGN_OP, EXPRESSION].
// `lvaloff` is the index of the first lvalue child (always 2 today;
// exposed so callers iterate as `ast.nodes[av.lvaloff + i]`). All four
// walkers (interp shadow / interp eval / JIT shadow / JIT compile) and
// both passes (collect_fn_locals, visit_for_frees) read through this
// view so a future grammar tweak only updates view_assignment.
struct AssignmentView {
  bool is_let;                  // node[0].token == "let"
  bool is_mut;                  // node[1].token == "mut"
  bool compound;                // op_token != "="
  std::string_view op_token;    // e.g. "+=", "*=", "="
  std::string_view op_base;     // op_token with trailing '=' stripped (compound), else ""
  std::string_view type_annotation;  // "" when absent
  int lvalcnt;                  // count of lvalue chain items
  size_t lvaloff;               // index of first lvalue child (= 2)
  const peg::Ast* rhs;          // node.back() — EXPRESSION
};

inline AssignmentView view_assignment(const peg::Ast& a) {
  std::string_view op_token = a.nodes[a.nodes.size() - 2]->token;
  bool compound = op_token != "=";
  auto type_annotation = extract_type_annotation(a, a.nodes.size() - 3);
  int lvalcnt = static_cast<int>(a.nodes.size()) - 4;
  if (!type_annotation.empty()) lvalcnt--;
  return AssignmentView{
      a.nodes[0]->token == "let",
      a.nodes[1]->token == "mut",
      compound,
      op_token,
      compound ? op_token.substr(0, op_token.size() - 1) : std::string_view{},
      type_annotation,
      lvalcnt,
      /*lvaloff=*/2,
      a.nodes.back().get(),
  };
}

// The single plain-name target of `x = v` / `let x = v` / `mut x = v` (and
// their compound forms), or nullptr when the assignment writes a complex
// lvalue (`o.a = v`, `a[i] = v`). Callers that must exclude compound
// assignment check `av.compound` themselves — a read analysis wants the name
// for `x += 1`, the passes that pick out declarations don't.
inline const peg::Ast* assign_name_target(const peg::Ast& a,
                                          const AssignmentView& av) {
  using namespace peg::udl;
  if (av.lvalcnt != 1) return nullptr;
  const auto& t = *a.nodes[av.lvaloff];
  return (t.tag == "IDENTIFIER"_ && t.is_token) ? &t : nullptr;
}

// View of a PLACE_ASSIGN AST node — see grammar:
//   PLACE_ASSIGN <- '(' PLACE (',' PLACE)+ ','? ')' '=' EXPRESSION
// Children are the targets followed by the RHS, so the target count is one
// less than the child count. Each target is a PLACE node (an lvalue chain) or,
// when it carries no postfix and the AstOptimizer collapsed it, the bare
// IDENTIFIER itself.
struct PlaceAssignView {
  size_t count;         // number of targets
  const peg::Ast* rhs;  // node.back() — EXPRESSION
};

inline PlaceAssignView view_place_assign(const peg::Ast& a) {
  return PlaceAssignView{a.nodes.size() - 1, a.nodes.back().get()};
}

// Split a PLACE_ASSIGN's targets into the two kinds every static walker has to
// treat differently: a chain target (`p[i]`, `o.a`) is a *read* of its own
// receiver and index subexpressions, while a plain-name target is a *write*
// with `x = v` semantics (reassign if visible, else declare). Calls
// `on_chain(node)` / `on_name(node)` per target; the RHS is not visited.
// `_` binds nothing, so it reaches neither callback — same rule as
// for_each_pattern_binding, which every one of these walkers also feeds from.
template <class OnChain, class OnName>
inline void for_each_place_target(const peg::Ast& a, OnChain on_chain,
                                  OnName on_name) {
  using namespace peg::udl;
  auto pv = view_place_assign(a);
  for (size_t i = 0; i < pv.count; i++) {
    const auto& t = *a.nodes[i];
    if (t.tag == "PLACE"_) {
      on_chain(t);
    } else if (t.token != "_") {
      on_name(t);
    }
  }
}

// A PLACE node's children are exactly an ASSIGNMENT's lvalue chain (a head
// followed by INDEX / DOT / ARGUMENTS postfixes), so both backends can hand
// one to their existing complex-lvalue assignment path by viewing it as a
// plain `chain = value`. The fields eval_assign_complex / compile_assign_complex
// read are the chain span plus the operator flags; there is no RHS node to
// point at (PLACE_ASSIGN evaluates its own RHS once for all targets).
inline AssignmentView view_place_as_assignment(const peg::Ast& place) {
  return AssignmentView{
      /*is_let=*/false,
      /*is_mut=*/false,
      /*compound=*/false,
      /*op_token=*/"=",
      /*op_base=*/std::string_view{},
      /*type_annotation=*/std::string_view{},
      /*lvalcnt=*/static_cast<int>(place.nodes.size()),
      /*lvaloff=*/0,
      /*rhs=*/nullptr,
  };
}

// A loop's optional trailing NOBREAK_CLAUSE node, or nullptr when absent.
// NOBREAK_CLAUSE is kept by the AstOptimizer, so a loop's last child is a
// NOBREAK_CLAUSE tag exactly when a `nobreak { … }` is present. Callers that
// need the clause node itself (e.g. to slice its source text) use this; those
// that want the inner block use nobreak_block_of below.
inline const peg::Ast* nobreak_clause_of(const peg::Ast& loop) {
  using namespace peg::udl;
  if (loop.nodes.empty()) return nullptr;
  const auto& last = *loop.nodes.back();
  return last.tag == "NOBREAK_CLAUSE"_ ? &last : nullptr;
}

// The BLOCK inside a loop's nobreak clause, or nullptr when the loop has none.
// Shared by view_while / view_for.
inline const peg::Ast* nobreak_block_of(const peg::Ast& loop) {
  const peg::Ast* clause = nobreak_clause_of(loop);
  return clause ? clause->nodes[0].get() : nullptr;
}

// The LOOP_LABEL node a loop or a break/continue carries, or nullptr when it
// has none. LOOP_LABEL is a token rule, so it is a leaf the AstOptimizer
// keeps, and it is always the node's first child — a loop's label precedes
// `while` / `for`, and a break/continue has no other child.
inline const peg::Ast* loop_label_of(const peg::Ast& a) {
  using namespace peg::udl;
  if (a.nodes.empty()) return nullptr;
  const auto& first = *a.nodes[0];
  return first.tag == "LOOP_LABEL"_ ? &first : nullptr;
}

// The label a BREAK / CONTINUE names, or an empty view when it has none.
inline std::string_view break_label_of(const peg::Ast& a) {
  const peg::Ast* label = loop_label_of(a);
  return label ? label->token : std::string_view{};
}

// A loop's label as an owned name, empty when unlabelled.
inline std::string loop_label_name(const peg::Ast* label) {
  return label ? std::string(label->token) : std::string();
}

// The `label: ` prefix a desugared loop carries over from the `for` it
// replaces, or nothing. Both FOR→WHILE rewrites emit it ahead of the `while`
// they synthesize, so a `break outer` in the body still names the loop.
inline std::string loop_label_prefix(const peg::Ast* label) {
  return label ? loop_label_name(label) + ": " : std::string();
}

// The two loop-label rejections, worded once. `culebra lint` hoists the check
// so it lands before the program runs, and the bytecode compiler makes it
// again as the backstop — the pair drifted apart the last time this was
// written out twice (`break outside loop` vs `break outside a loop`).
inline std::string no_such_loop_label_msg(std::string_view label) {
  return std::format("no enclosing loop labelled '{}'", label);
}

inline std::string duplicate_loop_label_msg(std::string_view label) {
  return std::format("duplicate loop label '{}'", label);
}

// View of a WHILE AST node — see grammar:
//   WHILE <- (LOOP_LABEL _ ':' _)? while _ (INIT_CLAUSE _ ';' _)? EXPRESSION _ BLOCK (_ NOBREAK_CLAUSE)?
// The label, the init clause and the nobreak clause are all optional, so the
// condition/body indices float. LOOP_LABEL / INIT_CLAUSE / NOBREAK_CLAUSE
// survive the AstOptimizer (a token leaf, and the parser.h keep-list), so
// their presence is a first-/last-child tag test. All WHILE consumers (the
// bytecode compiler, formatter, lint, transforms) read through this view so
// the grammar's optional-clause shape lives in exactly one place.
struct WhileView {
  const peg::Ast* label;    // LOOP_LABEL token node, or nullptr when absent
  const peg::Ast* init;     // INIT_CLAUSE node, or nullptr when absent
  const peg::Ast* cond;     // condition EXPRESSION
  const peg::Ast* body;     // body BLOCK
  const peg::Ast* nobreak;  // nobreak clause's BLOCK, or nullptr when absent
};

inline WhileView view_while(const peg::Ast& a) {
  using namespace peg::udl;
  const peg::Ast* label = loop_label_of(a);
  size_t off = label ? 1 : 0;
  const peg::Ast* init =
      a.nodes[off]->tag == "INIT_CLAUSE"_ ? a.nodes[off].get() : nullptr;
  if (init) off++;
  return WhileView{
      label,
      init,
      a.nodes[off].get(),
      a.nodes[off + 1].get(),
      nobreak_block_of(a),
  };
}

// View of a FOR AST node — see grammar:
//   FOR <- (LOOP_LABEL _ ':' _)? for _ FOR_BINDING _ in _ EXPRESSION _ BLOCK (_ NOBREAK_CLAUSE)?
// The binding/iterable/body indices shift by one when the loop is labelled,
// and the trailing nobreak clause floats — so every consumer goes through
// here rather than indexing children itself.
struct ForView {
  const peg::Ast* label;    // LOOP_LABEL token node, or nullptr when absent
  const peg::Ast* binding;  // FOR_BINDING / pattern / IDENTIFIER
  const peg::Ast* iter;     // iterable EXPRESSION
  const peg::Ast* body;     // body BLOCK
  const peg::Ast* nobreak;  // nobreak clause's BLOCK, or nullptr when absent
};

inline ForView view_for(const peg::Ast& a) {
  const peg::Ast* label = loop_label_of(a);
  size_t off = label ? 1 : 0;
  return ForView{
      label,
      a.nodes[off].get(),
      a.nodes[off + 1].get(),
      a.nodes[off + 2].get(),
      nobreak_block_of(a),
  };
}

// Structural decode of a RANGE node (`[start?] OP [end?] [BY_STEP]?`): the
// operator child sits at index 0 when there is no start, else index 1;
// either endpoint may be omitted (open-ended). A trailing BY_STEP node
// (kept un-collapsed by the AstOptimizer, see the keep-list below) carries
// the step expression as its lone child and is excluded from the end-bound
// slot. Returns the endpoint/step AST nodes (null when absent) without
// compiling them, so callers control evaluation order. Single source for the
// RANGE layout shared by the interpreter (eval_range), the JIT
// (compile_range, compile_for's counted fast path), and the bytecode-VM
// spike.
struct RangeLayout {
  const peg::Ast* start;  // null if open-started
  const peg::Ast* end;    // null if open-ended
  bool inclusive;
  const peg::Ast* step;  // null if no `by` clause (implies step 1)
};

inline RangeLayout decode_range_layout(const peg::Ast& ast) {
  using namespace peg::udl;
  size_t op_idx = (ast.nodes[0]->tag == "RANGE_OPERATOR"_) ? 0 : 1;
  size_t n = ast.nodes.size();
  bool has_step = n > 0 && ast.nodes[n - 1]->tag == "BY_STEP"_;
  size_t end_limit = has_step ? n - 1 : n;
  return {
      op_idx == 1 ? ast.nodes[0].get() : nullptr,
      op_idx + 1 < end_limit ? ast.nodes[op_idx + 1].get() : nullptr,
      ast.nodes[op_idx]->token == "..=",
      has_step ? ast.nodes[n - 1]->nodes[0].get() : nullptr,
  };
}

// View of an IF AST node — see grammar:
//   IF <- if _ (INIT_CLAUSE _ ';' _)? EXPRESSION _ BLOCK
//         (_ else _ if _ EXPRESSION _ BLOCK)* (_ else _ BLOCK)?
// Post-optimizer children: [(INIT_CLAUSE)?, cond, block, cond, block, …,
// (else-block)?] — a flat cond/block pairing with an optional trailing else.
// The optional leading init clause shifts that pairing, so every consumer
// iterates the arms from `arm_off` instead of 0. Also serves CONDITIONAL (the
// ternary `c ? a : b`, shape [cond, then, else]) which shares compile_if: a
// CONDITIONAL never carries an INIT_CLAUSE, so arm_off is 0 there.
struct IfView {
  const peg::Ast* init;   // INIT_CLAUSE node, or nullptr when absent
  size_t arm_off;         // index of the first condition (1 with init, else 0)
};

inline IfView view_if(const peg::Ast& a) {
  using namespace peg::udl;
  bool has_init = !a.nodes.empty() && a.nodes[0]->tag == "INIT_CLAUSE"_;
  return IfView{has_init ? a.nodes[0].get() : nullptr, has_init ? 1u : 0u};
}

// View of a trailing statement modifier — see grammar:
//   STATEMENT <- STATEMENT_BASE (_ (STMT_MODIFIER_IF / STMT_MODIFIER_UNLESS))?
// A STATEMENT node only survives AST optimization under its own "STATEMENT"
// tag (rather than collapsing onto its lone child) when the modifier is
// present, since that is the only shape with more than one child — so
// `has_postfix_modifier` doubles as the presence check. Both the parser's
// desugar_postfix_modifiers (which rewrites this shape into a plain IF node
// for the interp/JIT backends) and the formatter (which prints it verbatim
// as its own one-line shape) go through this view.
struct PostfixModifierView {
  const peg::Ast* base;      // the modified statement (STATEMENT_BASE result)
  const peg::Ast* cond;      // the modifier's condition (its lone child)
  bool is_unless;            // true for `unless`, false for `if`
};

inline bool has_postfix_modifier(const peg::Ast& stmt) {
  using namespace peg::udl;
  return stmt.tag == "STATEMENT"_ && stmt.nodes.size() == 2;
}

inline PostfixModifierView view_postfix_modifier(const peg::Ast& stmt) {
  using namespace peg::udl;
  const auto& modifier = *stmt.nodes[1];
  return PostfixModifierView{stmt.nodes[0].get(), modifier.nodes[0].get(),
                             modifier.tag == "STMT_MODIFIER_UNLESS"_};
}

// View of a MATCH AST node — see grammar:
//   MATCH <- match _ (INIT_CLAUSE _ ';' _)? EXPRESSION _ '{' _ MATCH_ARMS _ '}'
// Post-optimizer children: [(INIT_CLAUSE)?, subject, MATCH_ARMS]. The optional
// leading init clause shifts subject/arms by one, so consumers read them via
// this view instead of the fixed nodes[0]/nodes[1]. INIT_CLAUSE is kept by the
// AstOptimizer, so it is detectable as MATCH's first child (see view_if).
struct MatchView {
  const peg::Ast* init;      // INIT_CLAUSE node, or nullptr when absent
  const peg::Ast* subject;   // the value being matched
  const peg::Ast* arms;      // MATCH_ARMS node
};

inline MatchView view_match(const peg::Ast& a) {
  using namespace peg::udl;
  bool has_init = !a.nodes.empty() && a.nodes[0]->tag == "INIT_CLAUSE"_;
  size_t off = has_init ? 1u : 0u;
  return MatchView{has_init ? a.nodes[0].get() : nullptr,
                   a.nodes[off].get(), a.nodes[off + 1].get()};
}

// View of an OBJECT_PROPERTY AST node — see grammar:
//   OBJECT_PROPERTY <- MUTABLE _ (FLOAT/NUMBER/NIL/BOOLEAN/TUPLE/IDENTIFIER) _ ':' _ EXPRESSION
//   (shorthand) OBJECT_PROPERTY <- MUTABLE _ IDENTIFIER   (no ':' or value)
// Layouts: long form size 3 `[MUTABLE, KEY, EXPRESSION]`,
//          shorthand size 2 `[MUTABLE, IDENTIFIER]` (IDENTIFIER doubles
//          as the key and the read-from-scope value).
// Normalizing `value` (shorthand collapses to the identifier node) lets
// closure-capture walkers visit a single child without branching.
struct ObjectPropertyView {
  bool is_shorthand;            // size() == 2
  bool is_mut;                  // node[0].token == "mut"
  const peg::Ast* key;          // node[1]
  const peg::Ast* value;        // shorthand: same as key; long: node[2]
};

inline ObjectPropertyView view_object_property(const peg::Ast& p) {
  bool is_shorthand = p.nodes.size() == 2;
  return ObjectPropertyView{
      is_shorthand,
      p.nodes[0]->token == "mut",
      p.nodes[1].get(),
      is_shorthand ? p.nodes[1].get() : p.nodes[2].get(),
  };
}

// View of a TRAIT_METHOD AST node — see grammar:
//   TRAIT_METHOD <- IDENTIFIER _ PARAMETERS (_ RETURN_TYPE)? (_ TRAIT_BODY)?
// Both RETURN_TYPE and TRAIT_BODY are optional, so the body slot
// floats — the loop below tag-tests instead of indexing. `body` is an
// empty shared_ptr for signature-only methods (required-method form);
// when present it points at the inner BLOCK (TRAIT_BODY wraps BLOCK so
// AstOptimizer doesn't fold it).
struct TraitMethodView {
  std::string_view name;
  size_t name_line;
  size_t name_col;
  const peg::Ast* params;             // node[1]
  std::string_view return_type;       // "" when absent
  std::shared_ptr<peg::Ast> body;     // empty for sig-only methods
};

inline TraitMethodView view_trait_method(const peg::Ast& m) {
  using namespace peg::udl;
  const auto& ident = *m.nodes[0];
  std::shared_ptr<peg::Ast> body;
  std::string_view return_type;
  for (size_t j = 2; j < m.nodes.size(); j++) {
    if (m.nodes[j]->tag == "RETURN_TYPE"_) {
      return_type = m.nodes[j]->token;
    } else if (m.nodes[j]->tag == "TRAIT_BODY"_) {
      body = m.nodes[j]->nodes.empty() ? m.nodes[j] : m.nodes[j]->nodes[0];
    }
  }
  return TraitMethodView{
      ident.token, ident.line, ident.column,
      m.nodes[1].get(), return_type, body,
  };
}

// View of a METHOD AST node — see grammar:
//   METHOD <- MEMBER_MOD _ IDENTIFIER _
//               (TYPE_ANNOTATION (_ '=' _ EXPRESSION)? / '=' _ EXPRESSION
//                / PARAMETERS _ BLOCK)
// MEMBER_MOD (nodes[0]) holds at most one of `static` / `get`, so node
// indices are unchanged; a `get` method (nodes[2] == PARAMETERS) is read
// as a property. Three member forms, told apart by the tag of nodes[2]:
//   - typed instance field (`x: Float32`, `x: Float32 = 0.0`): nodes[2]
//     is TYPE_ANNOTATION; an optional default expression is nodes[3].
//   - static field (`static x = expr`): nodes[2] is the value expression.
//   - method (`f(a) { ... }`): nodes[2] is PARAMETERS, body is nodes[3].
// One central accessor avoids walker drift when the rule changes again.
struct MethodView {
  bool is_static;
  bool is_getter;                 // `get name() { ... }` — read as `obj.name`
  std::string_view name;
  size_t name_line;
  size_t name_col;
  bool is_field;                  // static field: `static x = expr`
  bool is_typed_field;            // typed instance field: `x: Type [= expr]`
  std::string_view type_annotation;  // typed field's type token; else empty
  const peg::Ast* params;         // nullptr unless method
  const std::shared_ptr<peg::Ast>* body;  // nullptr unless method
  const peg::Ast* value;          // static field value OR typed field
                                  // default; nullptr when neither
  const std::shared_ptr<peg::Ast>* value_sp;  // shared_ptr slot backing
                                  // `value` — instance-field initializers
                                  // outlive the declaration (evaluated per
                                  // instance), so holders keep the subtree
                                  // alive through this instead of the raw ptr
};

inline MethodView view_method(const peg::Ast& m) {
  using namespace peg::udl;
  const auto& ident = *m.nodes[1];
  // MEMBER_MOD (nodes[0]) carries at most one of `static` / `get`.
  bool is_static = m.nodes[0]->token == "static";
  bool is_getter = m.nodes[0]->token == "get";
  const auto& third = *m.nodes[2];
  if (third.tag == "TYPE_ANNOTATION"_) {
    bool has_default = m.nodes.size() == 4;
    return MethodView{
        is_static, is_getter, ident.token, ident.line, ident.column,
        /*is_field=*/false, /*is_typed_field=*/true,
        /*type_annotation=*/third.token,
        /*params=*/nullptr, /*body=*/nullptr,
        /*value=*/has_default ? m.nodes[3].get() : nullptr,
        /*value_sp=*/has_default ? &m.nodes[3] : nullptr,
    };
  }
  if (third.tag == "PARAMETERS"_) {
    return MethodView{
        is_static, is_getter, ident.token, ident.line, ident.column,
        /*is_field=*/false, /*is_typed_field=*/false, /*type_annotation=*/{},
        /*params=*/m.nodes[2].get(), /*body=*/&m.nodes[3], /*value=*/nullptr,
        /*value_sp=*/nullptr,
    };
  }
  // Bare `= EXPRESSION`: static field.
  return MethodView{
      is_static, is_getter, ident.token, ident.line, ident.column,
      /*is_field=*/true, /*is_typed_field=*/false, /*type_annotation=*/{},
      /*params=*/nullptr, /*body=*/nullptr, /*value=*/m.nodes[2].get(),
      /*value_sp=*/&m.nodes[2],
  };
}

// View of a VARIANT AST node — see grammar:
//   VARIANT <- IDENTIFIER (_ '(' (VARIANT_FIELD ...)? ')')?
// Index of the first non-DECORATOR child. CLASS_DECL / MULTIFN_DECL /
// ENUM_DECL / TRAIT_DECL carry leading `@deco` children before the real
// body; this localizes that AST-layout knowledge. The "skip leading
// decorators" loop was copy-pasted across interp/jit — a grammar tweak
// to decorator placement would otherwise need updating each copy. Only
// the pure index-advancing sites use this; sites that also recurse into
// the decorator nodes keep their own loop.
inline size_t first_non_decorator_index(const peg::Ast& node) {
  using namespace peg::udl;
  size_t i = 0;
  while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) i++;
  return i;
}

// True if this EFFECT_FN_DECL has a body (an effectful function) rather than a
// bare operation declaration. Grammar:
//   effect _ fn _ CLASS_HEAD _ PARAMETERS (_ RETURN_TYPE)? (_ BLOCK)?
// Children: [CLASS_HEAD, PARAMETERS, (RETURN_TYPE)?, (BODY)?]. The AstOptimizer
// collapses a single-statement BLOCK onto its statement, so the body may
// surface as STATEMENTS or a bare expression, not literally a BLOCK; RETURN_TYPE
// is keep-listed and keeps its tag. So a body is present iff there is a child
// past the two mandatory ones (CLASS_HEAD, PARAMETERS) whose tag is not
// RETURN_TYPE. Shared by the effects lowering and the lint pass so the shape
// knowledge lives in one place.
inline bool effect_fn_has_body(const peg::Ast& decl) {
  using namespace peg::udl;
  return decl.nodes.size() > 2 && decl.nodes.back()->tag != "RETURN_TYPE"_;
}

// `arity` is the positional payload count (0 = nullary). VARIANT is
// kept un-collapsed by the AstOptimizer so nodes[0] is always the name.
struct VariantView {
  std::string_view name;
  size_t name_line;
  size_t name_col;
  size_t arity;
  // The payload field type tokens (`VARIANT_FIELD <- < TYPE_REF >`), in order.
  // Documentation for plain enums; load-bearing for `@packable` enums, which
  // need each payload type to compute the fixed tagged-union layout.
  std::vector<std::string_view> field_types;
};
inline VariantView view_variant(const peg::Ast& v) {
  const auto& ident = *v.nodes[0];
  std::vector<std::string_view> types;
  for (size_t i = 1; i < v.nodes.size(); i++) types.push_back(v.nodes[i]->token);
  return VariantView{ident.token, ident.line, ident.column,
                     v.nodes.size() - 1, std::move(types)};
}

// Stable storage for synthetic positional payload field names
// (`_0`, `_1`, ...) so a FunctionValue::Parameter's string_view name
// and the instance field key outlive the call. Capped well above any
// realistic variant arity.
inline std::string_view positional_field_name(size_t i) {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    v.reserve(64);
    for (size_t k = 0; k < 64; k++) v.push_back("_" + std::to_string(k));
    return v;
  }();
  return names.at(i);
}

// An untyped instance field (`x = e`) has no declared type, so a @packable
// class — whose byte layout is computed FROM the field types — cannot
// accept it. The message is shared with the lint pass (which reports it
// pre-eval as a Diagnostic rather than a throw) so the two can't drift.
inline std::string packable_untyped_field_message(std::string_view name,
                                                  std::string_view class_name) {
  return std::format("field `{}` in @packable class `{}` needs a type "
                     "annotation (the fixed layout is computed from it)",
                     name, class_name);
}
// Evaluator-side safety net: throws a single canonical SyntaxError so interp
// and JIT diagnostics stay identical.
inline void require_typed_packable_field(const MethodView& mv,
                                         std::string_view class_name) {
  throw CulebraError("SyntaxError",
                     packable_untyped_field_message(mv.name, class_name),
                     static_cast<long>(mv.name_line),
                     static_cast<long>(mv.name_col));
}

// A getter (`get name() { ... }`) takes no parameters — it is read as a
// property, so there is no call site to pass arguments. Getter fields are
// rejected earlier by the grammar's `&IdentInitChar` lookahead never firing
// on `get x = ...` / `get x: T`, which is why a null `params` counts as a
// violation here.
inline bool getter_takes_params(const MethodView& mv) {
  return !(mv.params && mv.params->nodes.empty());
}

// The message is shared with the lint pass (which reports it pre-eval as a
// Diagnostic rather than a throw) so the two can't drift.
inline std::string getter_params_message(std::string_view name,
                                         std::string_view class_name) {
  return std::format("getter `{}` in class `{}` must take no parameters",
                     name, class_name);
}

// Evaluator-side safety net: throws the same canonical SyntaxError.
inline void require_getter_no_params(const MethodView& mv,
                                     std::string_view class_name) {
  if (!getter_takes_params(mv)) return;
  throw CulebraError("SyntaxError",
                     getter_params_message(mv.name, class_name),
                     static_cast<long>(mv.name_line),
                     static_cast<long>(mv.name_col));
}

// First `...` spread element directly inside a sized array literal
// (`[v, ...x](n)`), or nullptr. Only the sized array's own element list
// counts — a spread inside a nested plain literal is fine.
inline const peg::Ast* find_sized_spread_mix(const peg::Ast& ast) {
  using namespace peg::udl;
  if (ast.tag == "ARRAY"_ && ast.nodes.size() > 1) {
    for (const auto& e : ast.nodes[0]->nodes) {
      if (e->tag == "SPREAD_ELEM"_) return e.get();
    }
  }
  for (const auto& n : ast.nodes) {
    if (auto* hit = find_sized_spread_mix(*n)) return hit;
  }
  return nullptr;
}

// A sized array literal cannot contain a spread: the size prefill (resize,
// then literals overwrite positions 0..) and a spread's append compose into
// a shape whose size no longer matches the count, so the mix is rejected at
// parse time on every backend.
inline void reject_sized_spread_mix(const peg::Ast& ast) {
  if (auto* s = find_sized_spread_mix(ast)) {
    throw CulebraError(
        "SyntaxError",
        "a sized array literal (`[v](n)`) cannot contain a `...` spread "
        "element — spread into a plain `[...]` literal instead",
        static_cast<long>(s->line), static_cast<long>(s->column));
  }
}

// View of a `@derive(...)` decorator. The grammar is
//   DECORATOR <- '@' _ CALL
// and `@derive(Eq, Hash, Show, Comparable)` parses (post-AstOptimizer,
// DECORATOR kept un-collapsed) to:
//   DECORATOR
//     CALL
//       IDENTIFIER "derive"      (callee — PRIMARY collapsed)
//       ARG_LIST [ IDENTIFIER "Eq", IDENTIFIER "Hash", ... ]
// Returns the requested trait identifiers when the decorator is a bare
// `derive(...)` call, or an empty vector otherwise (a regular decorator
// the caller applies as a function). The eval/compile side validates the
// names against the supported set. Used by both interp (`eval_class_decl`)
// and JIT (`compile_class_decl`) so the directive is recognized
// identically.
inline std::vector<std::string_view> view_derive(const peg::Ast& decorator) {
  std::vector<std::string_view> traits;
  if (decorator.nodes.empty()) return traits;
  const auto& call = *decorator.nodes[0];
  // A derive directive is `derive(...)`: a 2-child CALL whose callee is
  // the bare identifier `derive` and whose arg list follows.
  if (call.nodes.size() < 2) return traits;
  if (call.nodes[0]->token != "derive") return traits;
  for (const auto& arg : call.nodes[1]->nodes) {
    traits.push_back(arg->token);
  }
  return traits;
}

// Detect a bare compiler-directive decorator by name. Like `@derive`, these
// are not callables applied to the class — each flips the declaration into a
// different kind. The CALL collapses to the identifier when there are no
// args, so the decorator's child is either the bare identifier or the CALL's
// callee.
inline bool is_directive_decorator(const peg::Ast& decorator,
                                   std::string_view name) {
  if (decorator.nodes.empty()) return false;
  const auto& c = *decorator.nodes[0];
  return (c.nodes.empty() ? c.token : c.nodes[0]->token) == name;
}
// `@packable`: a fixed-layout struct (declared fields only, fixed scalar
// types, no dynamic fields) whose bytes SharedBuffer shares across isolates.
inline bool is_packable_decorator(const peg::Ast& decorator) {
  return is_directive_decorator(decorator, "packable");
}
// `@value`: a class whose instances have no identity — declared scalar
// fields, no `drop`, frozen once `new` returns, compared by field.
inline bool is_value_decorator(const peg::Ast& decorator) {
  return is_directive_decorator(decorator, "value");
}
// The three the compiler reads itself rather than calls. apply_decorators
// skips them, so a declaration carrying only these is one whose value no
// decorator can replace — which is what lets a construction site name the
// constructor it reaches.
inline bool is_compile_time_decorator(const peg::Ast& decorator) {
  return is_packable_decorator(decorator) || is_value_decorator(decorator) ||
         !view_derive(decorator).empty();
}

// Registry of `@value` class names, so a later `@value` class can declare a
// field of one (`a: Vector2`). Written where a declaration is first seen —
// lint pre-eval, and compile_class_decl as its safety net — the same
// two-writer arrangement @packable's layout registry has, and with the same
// limitation: a name stays registered for the life of the process.
inline std::mutex& value_class_mutex() {
  static std::mutex m;
  return m;
}
inline std::set<std::string, std::less<>>& value_class_registry() {
  static std::set<std::string, std::less<>> reg;
  return reg;
}
inline void register_value_class(std::string name) {
  std::lock_guard<std::mutex> lk(value_class_mutex());
  value_class_registry().insert(std::move(name));
}
inline bool is_value_class(std::string_view name) {
  std::lock_guard<std::mutex> lk(value_class_mutex());
  return value_class_registry().contains(name);
}

// The declared field names, in order, of a `@value` class flat enough to
// unbox into that many ordinary slots: at least one field, every one a
// machine scalar (no nested `@value` — that layout is a later step), and no
// initializer expression (an initializer routes construction through the
// field-init thunk, which is a call, and a call is what unboxing is here to
// remove). Registered from the same place and under the same rule as the
// name above; absent means "not eligible", which is always a safe answer.
inline std::map<std::string, std::vector<std::string>, std::less<>>&
value_flat_layouts() {
  static std::map<std::string, std::vector<std::string>, std::less<>> reg;
  return reg;
}
inline void register_value_flat_layout(std::string name,
                                       std::vector<std::string> fields) {
  std::lock_guard<std::mutex> lk(value_class_mutex());
  value_flat_layouts().insert_or_assign(std::move(name), std::move(fields));
}
// The layout by class name, or nullptr when the class is not flat-eligible.
// Returns a pointer into the registry, which only ever grows, so the callee
// (the compiler, single-threaded over one module list) may hold it.
inline const std::vector<std::string>* value_flat_layout(
    std::string_view name) {
  std::lock_guard<std::mutex> lk(value_class_mutex());
  auto it = value_flat_layouts().find(name);
  return it == value_flat_layouts().end() ? nullptr : &it->second;
}

// The declared type of each field of a class (culebra::FieldType as a byte),
// by class name then field name. Every class registers its own, `@value` or
// not: a field's declared *scalar* type is checked on every write
// (docs/language.md §10), so a reader that knows the receiver's class knows
// the type of such a field. Every other annotation is FieldType::Any and
// never lands here. Absent means "ask at run time", always a safe answer.
using ClassFieldTypes = std::map<std::string, uint8_t, std::less<>>;
inline std::map<std::string, ClassFieldTypes, std::less<>>&
class_field_types() {
  static std::map<std::string, ClassFieldTypes, std::less<>> reg;
  return reg;
}
inline void register_class_field_types(std::string name,
                                       ClassFieldTypes fields) {
  std::lock_guard<std::mutex> lk(value_class_mutex());
  class_field_types().insert_or_assign(std::move(name), std::move(fields));
}
// The table by class name, or nullptr when the class declared no typed
// field. The registry only ever grows, so the caller may hold the pointer.
inline const ClassFieldTypes* class_field_types_of(std::string_view name) {
  std::lock_guard<std::mutex> lk(value_class_mutex());
  auto it = class_field_types().find(name);
  return it == class_field_types().end() ? nullptr : &it->second;
}

// The class a field's annotation names, by class name then field name —
// the same registration as above for a field whose declared type is not a
// scalar. `self.car.speed` needs it: the first step says the second step's
// receiver is a Car, and Car says what `speed` is.
using ClassFieldClasses = std::map<std::string, std::string, std::less<>>;
inline std::map<std::string, ClassFieldClasses, std::less<>>&
class_field_classes() {
  static std::map<std::string, ClassFieldClasses, std::less<>> reg;
  return reg;
}
inline void register_class_field_classes(std::string name,
                                         ClassFieldClasses fields) {
  std::lock_guard<std::mutex> lk(value_class_mutex());
  class_field_classes().insert_or_assign(std::move(name), std::move(fields));
}
inline const ClassFieldClasses* class_field_classes_of(std::string_view name) {
  std::lock_guard<std::mutex> lk(value_class_mutex());
  auto it = class_field_classes().find(name);
  return it == class_field_classes().end() ? nullptr : &it->second;
}

// A `@value` field holds a machine scalar or another `@value`. Everything
// else — String, Array, Object, a closure, `T?`, an ordinary class — carries
// a heap body or an identity of its own, so admitting it would give the
// contract nothing to stand on. Deliberately narrow: widening this set later
// is additive, narrowing it is not.
inline bool is_value_field_type(std::string_view t) {
  return t == "Long" || t == "Float" || t == "Bool" || is_value_class(t);
}

// The machine-scalar half of the above: a field an unboxed layout can give
// exactly one slot. A nested `@value` field is a legal value field but needs
// its own fields flattened in, which is a later step — so it answers no here
// while still answering yes to is_value_field_type.
inline bool is_flat_value_field_type(std::string_view t) {
  return t == "Long" || t == "Float" || t == "Bool";
}

// The `@value` contract's declaration-time messages. Shared with the lint
// pass (which reports them pre-eval as Diagnostics rather than throws) so the
// two can't drift, exactly as the @packable pair above.
inline std::string value_untyped_field_message(std::string_view name,
                                               std::string_view class_name) {
  return std::format("field `{}` in @value class `{}` needs a type annotation "
                     "(a value's field set is fixed at the declaration)",
                     name, class_name);
}
inline std::string value_field_type_message(std::string_view class_name,
                                            std::string_view name,
                                            std::string_view type) {
  return std::format("@value class `{}`: field `{}` has non-value type `{}` "
                     "(expected Long, Float, Bool, or another @value class)",
                     class_name, name, type);
}
inline std::string value_drop_message(std::string_view class_name) {
  return std::format("@value class `{}` cannot define `drop` (a destructor "
                     "observes which instance died, and a value has no "
                     "identity to tell one from a copy)",
                     class_name);
}
// Rejected rather than silently skipped, for the same reason @derive is: a
// variant's positional payload has no declaration for the contract to read.
inline std::string value_on_enum_message(std::string_view enum_name) {
  return std::format("@value: not applicable to enum `{}` (its variants carry "
                     "no declared fields to hold to the contract)",
                     enum_name);
}
inline std::string value_packable_message(std::string_view class_name) {
  return std::format("class `{}` cannot be both @value and @packable "
                     "(@packable instances alias shared bytes, which is the "
                     "identity @value removes)",
                     class_name);
}

inline std::string value_undeclared_self_write_message(
    std::string_view class_name, std::string_view field) {
  return std::format("@value class `{}`: `self.{}` writes a field the class "
                     "does not declare (a value's field set is fixed at the "
                     "declaration, so two instances cannot differ in shape)",
                     class_name, field);
}

// The write that would break the clause above: `self.<name> = ...` for a name
// the class does not declare. In `new` it gives that instance a field its
// siblings lack; in a method it is the ImmutableError the freeze raises, and
// saying so at the declaration is both earlier and more specific. Only a
// literal one-step `self.NAME` target is a shape the declaration can see —
// `self[expr] = v` and a write through an alias are not, and are refused at
// run time instead (JitObject::fields_closed, set from the allocation).
//
// A nested class declaration binds a `self` of its own, so the walk stops
// there. A `static` member's `self` is the class object rather than an
// instance, and is exempt for the same reason require_value_member exempts it.
struct ValueSelfWrite {
  std::string_view field;
  size_t line = 0;
  size_t col = 0;
};
inline void find_value_self_writes(
    const peg::Ast& node, const std::set<std::string, std::less<>>& declared,
    std::vector<ValueSelfWrite>& out) {
  using namespace peg::udl;
  if (node.tag == "CLASS_DECL"_) return;
  auto check_chain = [&](const peg::Ast& owner, const AssignmentView& av) {
    if (av.lvalcnt != 2) return;
    const auto& head = *owner.nodes[av.lvaloff];
    const auto& post = *owner.nodes[av.lvaloff + 1];
    if (head.tag != "IDENTIFIER"_ || head.token != "self") return;
    if (post.original_tag != "DOT"_) return;
    if (declared.contains(post.token)) return;
    out.push_back({post.token, post.line, post.column});
  };
  if (node.tag == "ASSIGNMENT"_) {
    check_chain(node, view_assignment(node));
  } else if (node.tag == "PLACE_ASSIGN"_) {
    for_each_place_target(
        node,
        [&](const peg::Ast& t) { check_chain(t, view_place_as_assignment(t)); },
        [](const peg::Ast&) {});
  }
  for (const auto& n : node.nodes) find_value_self_writes(*n, declared, out);
}

// The members the clause applies to, in source order. A `static` member's
// `self` is the class object rather than an instance, exempt for the same
// reason require_value_member exempts it — so lint and the compiler ask the
// question over the same set rather than each writing the rule out.
inline std::vector<ValueSelfWrite> find_value_self_writes_in_members(
    const peg::Ast& class_ast, size_t first_member,
    const std::set<std::string, std::less<>>& declared) {
  std::vector<ValueSelfWrite> out;
  for (size_t i = first_member; i < class_ast.nodes.size(); i++) {
    auto mv = view_method(*class_ast.nodes[i]);
    if (mv.is_static && !mv.is_typed_field) continue;
    find_value_self_writes(*class_ast.nodes[i], declared, out);
  }
  return out;
}

// Whether a member body is straight-line enough to compile into a caller
// instead of a frame of its own: no control flow that would need a chunk's
// jump/unwind machinery re-established at the splice. `RETURN` is refused
// rather than handled because an early return out of an inlined body would
// have to become a jump past the rest of the CALLER's expression, which is
// not something an expression position can express.
//
// The walk stops at a nested declaration for the same reason
// find_value_self_writes does: an inner `fn` or `class` brings its own body,
// compiled as its own chunk and not spliced here. Such a literal is refused
// separately (value_body_has_nested_fn) — this predicate answers only about
// the body's own control flow.
inline bool is_straightline_body(const peg::Ast& node) {
  using namespace peg::udl;
  switch (node.tag) {
    case "FOR"_:
    case "WHILE"_:
    case "RETURN"_:
    case "TRY"_:
    case "DEFER"_:
      return false;
    case "CLASS_DECL"_:
    case "ENUM_DECL"_:
    case "FUNCTION"_:
    case "MULTIFN_DECL"_:
      return true;  // its own chunk; not spliced, so its shape is not ours
    default:
      break;
  }
  for (const auto& n : node.nodes)
    if (!is_straightline_body(*n)) return false;
  return true;
}

// Whether the body builds a closure or a class of its own. Such a body is
// refused for inlining: the nested literal's capture list was resolved
// against the CALLEE's analysis (FuncInfo::captured_locals decides which of
// the callee's locals live in cells), and compiling it in the caller would
// consult the caller's analysis instead — a silently wrong cell decision
// rather than a visible failure.
inline bool value_body_has_nested_fn(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "FUNCTION"_ || node.tag == "MULTIFN_DECL"_ ||
      node.tag == "CLASS_DECL"_ || node.tag == "ENUM_DECL"_)
    return true;
  for (const auto& n : node.nodes)
    if (value_body_has_nested_fn(*n)) return true;
  return false;
}

// Whether the body writes `self.<name>` anywhere. A constructor's writes are
// the point (they are what lands in the unboxed slots); any OTHER member's
// would be the freeze's ImmutableError on a boxed instance, and reproducing
// that from an inlined body is machinery worth declining instead. Reuses the
// contract scan, asking it for "any field at all" with an empty declared set.
inline bool value_body_writes_self(const peg::Ast& body) {
  std::set<std::string, std::less<>> none;
  std::vector<ValueSelfWrite> out;
  find_value_self_writes(body, none, out);
  return !out.empty();
}

// Evaluator-side safety net for the per-member half of the contract: no
// `drop`, and every instance field declared with a value type. A `static`
// member other than a typed field lives on the class object, outside the
// instance protocol and its field set, so it is exempt. Throws the canonical
// SyntaxError at the member's name, matching what lint reports pre-eval.
inline void require_value_member(const MethodView& mv,
                                 std::string_view class_name) {
  if (mv.is_static && !mv.is_typed_field) return;
  auto at = [&](std::string msg) {
    return CulebraError("SyntaxError", std::move(msg),
                        static_cast<long>(mv.name_line),
                        static_cast<long>(mv.name_col));
  };
  if (mv.name == "drop") throw at(value_drop_message(class_name));
  if (mv.is_typed_field && !is_value_field_type(mv.type_annotation))
    throw at(value_field_type_message(class_name, mv.name,
                                      mv.type_annotation));
  if (mv.is_field) throw at(value_untyped_field_message(mv.name, class_name));
}

// Resolve a `@derive` trait name to the method it generates and the
// runtime selector the JIT uses (`make_derived_method`). Throws the
// canonical SyntaxError on an unknown name so interp / JIT diagnostics
// stay identical — same rationale as require_static_field. Shared by
// both backends.
struct DerivedMethod {
  std::string_view name;  // generated method name (static-lifetime literal)
  int kind;               // 0=eq, 1=hash, 2=show, 3=cmp
};
// nullopt for a name outside the derivable set. The static lint pass uses
// this form to report pre-eval (like the @packable field types), which is
// what keeps the diagnostic's *timing* backend-independent; the throwing
// form below stays as each backend's safety net.
inline std::optional<DerivedMethod> find_derive_method(std::string_view trait) {
  if (trait == "Eq") return DerivedMethod{"eq", 0};
  if (trait == "Hash") return DerivedMethod{"hash", 1};
  if (trait == "Show") return DerivedMethod{"to_s", 2};
  if (trait == "Comparable") return DerivedMethod{"cmp", 3};
  return std::nullopt;
}
inline std::string derive_unknown_trait_message(std::string_view trait) {
  return std::format("@derive: unknown trait `{}` (expected Eq, Hash, "
                     "Show, or Comparable)",
                     trait);
}
inline DerivedMethod derive_method_for(std::string_view trait) {
  if (auto dm = find_derive_method(trait)) return *dm;
  throw CulebraError("SyntaxError", derive_unknown_trait_message(trait));
}
// Rejected rather than silently skipped (see enum_variant_conforms_to_trait).
inline std::string derive_on_enum_message(std::string_view enum_name) {
  return std::format("@derive: not applicable to enum `{}` (its variants "
                     "are Eq and Hashable by construction)",
                     enum_name);
}

// Reserved words that may not name a variable. Single source for the
// assignment LHS check, shared by the interpreter and the static lint pass
// (an assignment target that is a keyword is a SyntaxError on every backend).
// The declaration introducers are here too: the PEG would happily backtrack to
// IDENTIFIER for them, but a variable named `class` or `import` reads as the
// construct. The rest of the grammar's keywords stay contextual (see the
// Keywords section of docs/language.md).
inline bool is_keyword(std::string_view ident) {
  using namespace std::literals;
  static const std::set<std::string_view> keywords = {
      "nil"sv,    "true"sv,   "false"sv,  "mut"sv,      "debugger"sv,
      "return"sv, "while"sv,  "for"sv,    "in"sv,       "if"sv,
      "unless"sv, "else"sv,   "fn"sv,     "match"sv,    "throw"sv,
      "try"sv,    "catch"sv,  "break"sv,  "continue"sv, "defer"sv,
      "class"sv,  "trait"sv,  "enum"sv,   "import"sv,   "export"sv,
      "yield"sv};
  return keywords.contains(ident);
}

// An assignment target that names a variable: an identifier that is not a
// reserved word. Single source for the interpreter's runtime guard and the
// static lint pass, like is_keyword itself.
inline bool is_assignable_name(const peg::Ast& node) {
  using namespace peg::udl;
  return node.tag == "IDENTIFIER"_ && !is_keyword(node.token);
}

// A malformed call argument list, detected purely from the AST: a positional
// argument after a keyword/`**` splat (SyntaxError) or a duplicate keyword
// (TypeError). Single source of these two rules, shared by the interpreter and
// the JIT so every call kind reports the same catchable error at the same
// position regardless of callee.
struct ArgListError {
  std::string kind;       // "SyntaxError" | "TypeError"
  std::string message;
  size_t line;
  size_t col;
};

// Scan a call's ARG_LIST (children are KWARG / KWARG_SPLAT / a positional
// expression). positional-after-keyword (SyntaxError) takes priority over a
// duplicate keyword (TypeError), matching the interpreter's split_call_args
// (which throws the former during its linear scan and only then checks dups).
inline std::optional<ArgListError> check_arg_list(const peg::Ast& args_ast) {
  using namespace peg::udl;
  bool saw_named = false;
  for (const auto& child : args_ast.nodes) {
    if (child->tag == "KWARG"_ || child->tag == "KWARG_SPLAT"_) {
      saw_named = true;
    } else if (saw_named) {
      return ArgListError{"SyntaxError",
                          "positional argument follows keyword argument",
                          child->line, child->column};
    }
  }
  std::set<std::string_view> seen;
  for (const auto& child : args_ast.nodes) {
    if (child->tag != "KWARG"_) continue;
    std::string_view name = child->nodes[0]->token;
    if (!seen.insert(name).second) {
      return ArgListError{
          "TypeError",
          std::format("duplicate keyword argument '{}'", name), child->line,
          child->column};
    }
  }
  return std::nullopt;
}

// An ARG_LIST split into its three buckets.
struct ArgScan {
  std::vector<const peg::Ast*> positional;
  std::vector<std::pair<std::string_view, const peg::Ast*>> explicit_kwargs;
  std::vector<const peg::Ast*> splats;  // `**` operands, in source order

  // The value expression given for `name`, or null. First wins, matching
  // the binder. Callers that skipped check_arg_list can be handed a repeat,
  // so they either decline one first (the sort family gates on a single
  // kwarg) or ask presence only (the builtin arity check).
  const peg::Ast* kwarg(std::string_view name) const {
    for (const auto& [kw, val] : explicit_kwargs)
      if (kw == name) return val;
    return nullptr;
  }
};

// Pure bucketing: it never reports a malformed list. Callers come in three
// kinds: most run check_arg_list first, which raises the structural errors —
// duplicate keyword, positional after keyword — with a precedence a linear
// scan here would get wrong; the builtin-method fast paths skip it and
// decline any shape they do not recognise; and the builtin arity check skips
// it and is itself the report.
inline ArgScan scan_arg_list(const peg::Ast& args_ast) {
  using namespace peg::udl;
  ArgScan out;
  out.positional.reserve(args_ast.nodes.size());
  for (auto& child : args_ast.nodes) {
    if (child->tag == "KWARG_SPLAT"_) {
      out.splats.push_back(child->nodes[0].get());
    } else if (child->tag == "KWARG"_) {
      out.explicit_kwargs.emplace_back(child->nodes[0]->token,
                                       child->nodes[1].get());
    } else {
      out.positional.push_back(child.get());
    }
  }
  return out;
}

// `_` is the non-binding sink in patterns and parameters.
inline bool is_sink_name(std::string_view s) { return s == "_"; }

// Names the runtime binds implicitly, never via a user declaration: `self`
// (the method receiver) / `fn` (the current function, for recursion), and the
// reserved dunder names injected per scope — `__ARGS__` / `__KWARGS__`,
// `__LINE__` / `__COLUMN__`, `__cls__`, and any future `__x__`. The whole
// `__x__` space counts, so the rule can't drift as new ones are added.
inline bool is_always_bound_name(std::string_view n) {
  if (n == "self" || n == "fn") return true;
  return n.size() > 4 && n.starts_with("__") && n.ends_with("__");
}

// The first name a pattern subtree binds, or nullptr. Bindings are non-sink
// IDENTIFIER / TYPED_IDENT / `...rest` leaves, wherever they sit; an object
// pattern's key names a field rather than a binding, so only its value
// sub-pattern is walked.
inline const peg::Ast* find_pattern_binding(const peg::Ast& pat) {
  using namespace peg::udl;
  switch (pat.tag) {
    case "IDENTIFIER"_:
      return is_sink_name(pat.token) ? nullptr : &pat;
    case "TYPED_IDENT"_:
    case "REST_PATTERN"_:
      return is_sink_name(pat.nodes[0]->token) ? nullptr : pat.nodes[0].get();
    case "OBJECT_PAT_ENTRY"_:  // `key: PATTERN` — the key is not a binding
      return find_pattern_binding(*pat.nodes[1]);
    default:
      break;
  }
  for (const auto& n : pat.nodes) {
    if (auto* hit = find_pattern_binding(*n)) return hit;
  }
  return nullptr;
}

// First binding inside an or-pattern (a PATTERN node keeps its children only
// when it has two or more alternatives), or nullptr.
inline const peg::Ast* find_or_pattern_binding(const peg::Ast& ast) {
  using namespace peg::udl;
  if (ast.tag == "PATTERN"_ && !ast.nodes.empty()) {
    if (auto* b = find_pattern_binding(ast)) return b;
  }
  for (const auto& n : ast.nodes) {
    if (auto* hit = find_or_pattern_binding(*n)) return hit;
  }
  return nullptr;
}

// An or-pattern alternative cannot bind: the name would exist only on the
// paths whose alternative matched, so every consumer has to invent a rule for
// the others (the interp leaves it undeclared, a two-phase test/bind split
// cannot express it at all). Rejected at parse time on every backend.
inline void reject_or_pattern_binding(const peg::Ast& ast) {
  if (auto* b = find_or_pattern_binding(ast)) {
    throw CulebraError(
        "SyntaxError",
        std::format("or-pattern alternatives cannot bind a name ('{}') — "
                    "write one pattern (or `match` arm) per alternative",
                    b->token),
        static_cast<long>(b->line), static_cast<long>(b->column));
  }
}

// `static new(...)` is a contradiction: `new` names the constructor, which
// builds an instance and binds `self` to it, and a static member has neither.
// Both existing backends sorted it into the statics and then disagreed about
// which binding of the class object's `new` property survived (the interp
// kept the synthesized constructor, the JIT let the static overwrite it), so
// the form has never meant one thing. Rejected at parse time on every
// backend rather than picking a winner for code nobody can have relied on.
inline void reject_static_new(const peg::Ast& ast) {
  using namespace peg::udl;
  if (ast.tag == "METHOD"_ && ast.nodes[0]->token == "static" &&
      ast.nodes[1]->token == "new" && ast.nodes.size() > 2 &&
      ast.nodes[2]->tag == "PARAMETERS"_) {
    const auto& id = *ast.nodes[1];
    throw CulebraError(
        "SyntaxError",
        "`new` cannot be static — the constructor always builds an instance; "
        "give the factory another name",
        static_cast<long>(id.line), static_cast<long>(id.column));
  }
  for (const auto& n : ast.nodes) reject_static_new(*n);
}

inline bool is_kw_only_sep(const peg::Ast& node) {
  using namespace peg::udl;
  return node.tag == "KW_ONLY_SEP"_;
}

inline bool is_kwargs_rest(const peg::Ast& node) {
  using namespace peg::udl;
  return node.tag == "KWARGS_REST"_;
}

// A positional catch-all (`*args`): collects positional arguments beyond
// the declared regular params into an Array. Counterpart to KWARGS_REST.
inline bool is_args_rest(const peg::Ast& node) {
  using namespace peg::udl;
  return node.tag == "ARGS_REST"_;
}

// A destructuring pattern used as a binding target: `fn ({a, b})` params
// and `for (k, v) in …` loop bindings both route through here. FOR_BINDING
// (the multi-target `for k, v` node) has the same shape as a tuple pattern
// and only ever appears as a loop binding, never in a real parameter list —
// so recognizing it here classifies for-bindings without affecting params.
inline bool is_pattern_param(const peg::Ast& node) {
  using namespace peg::udl;
  return node.tag == "OBJECT_PATTERN"_ || node.tag == "ARRAY_PATTERN"_ ||
         node.tag == "TUPLE_PATTERN"_ || node.tag == "FOR_BINDING"_;
}

// Stable synthetic names for destructured parameters (`fn ({a,b})` binds
// the arg to `__destructure_N`, then the body unpacks it).
inline std::string_view destructure_param_name(size_t i) {
  // deque keeps element addresses stable as it grows, so the returned
  // string_view stays valid after the lock is dropped; grow on demand rather
  // than capping. Locked because isolates parse on their own threads at the
  // same time, and two unsynchronised grows corrupt the deque.
  static std::mutex m;
  static std::deque<std::string> names;
  std::lock_guard<std::mutex> lk(m);
  while (names.size() <= i)
    names.push_back("__destructure_" + std::to_string(names.size()));
  return names[i];
}

// Positional arity of a trait method's parameter list. `has_param` reports
// what the interp's FunctionValue::params would hold — everything but the
// `*` separator — which is what the well-known contract check reads.
inline size_t trait_method_arity(const peg::Ast& params,
                                 bool* has_param = nullptr) {
  size_t arity = 0;
  if (has_param) *has_param = false;
  for (const auto& p : params.nodes) {
    if (is_kw_only_sep(*p)) continue;
    if (has_param) *has_param = true;
    if (is_kwargs_rest(*p) || is_args_rest(*p)) continue;
    arity++;
  }
  return arity;
}

// A trait declaration reduced to the constant strings the compiled
// backends hand to culebra_runtime_register_trait when the declaration
// executes: `name:arity:has_default;...` and `Super;...`.
struct TraitDeclSpec {
  std::string name;
  std::string methods;
  std::string supers;
};

inline TraitDeclSpec trait_decl_spec(const peg::Ast& ast) {
  TraitDeclSpec spec;
  size_t k = first_non_decorator_index(ast);
  auto th = parse_trait_head(ast.nodes[k]->token);
  spec.name = std::string(parse_generic_head(th.name).outer);
  for (auto super : th.supertraits) {
    spec.supers += std::string(super);
    spec.supers += ';';
  }
  for (size_t i = k + 1; i < ast.nodes.size(); i++) {
    auto tv = view_trait_method(*ast.nodes[i]);
    spec.methods += std::string(tv.name);
    spec.methods += ':';
    spec.methods += std::to_string(trait_method_arity(*tv.params));
    spec.methods += tv.body ? ":1;" : ":0;";
  }
  return spec;
}

// Returns (name, line, col) for a PARAMETER-shaped AST node. The
// KWARGS_REST shape stores the name as the node's own token; normal
// parameters keep it on the IDENTIFIER child at index 1.
struct ParamNameLoc {
  std::string_view name;
  size_t line;
  size_t column;
};
inline ParamNameLoc extract_param_name_loc(const peg::Ast& p) {
  if (is_kwargs_rest(p) || is_args_rest(p)) return {p.token, p.line, p.column};
  const auto& id = *p.nodes[1];
  return {id.token, id.line, id.column};
}

// Returns the index of the first kw-only parameter (the cap on
// allowed positional arguments), or nullopt if there is no `*`
// separator. Templated so it works for both FunctionValue::Parameter
// (interp) and JIT-local ParamInfo, which share a `kw_only` field.
template <class P>
inline std::optional<size_t> first_kw_only_index(
    const std::vector<P>& params) {
  for (size_t i = 0; i < params.size(); i++) {
    if (params[i].kw_only) return i;
  }
  return std::nullopt;
}

// Count of regular (positional-bindable) params — those with neither
// `kw_only` nor `kwargs_rest` set. This is where overflow positionals
// stop being consumed by formal slots and start flowing to __ARGS__.
template <class P>
inline size_t regular_param_count(const std::vector<P>& params) {
  size_t n = 0;
  for (auto& p : params) {
    if (!p.kw_only && !p.kwargs_rest && !p.args_rest) n++;
  }
  return n;
}

// True if the parameter list declares a `*args` positional catch-all.
// When present, there is no cap on positional arguments — the overflow
// flows into the `*args` Array rather than being rejected.
template <class P>
inline bool has_args_rest(const std::vector<P>& params) {
  for (auto& p : params) {
    if (p.args_rest) return true;
  }
  return false;
}

// Positional arity bounds for a built-in (fixed-shape) method, derived
// from its declared parameters — the single source of truth both
// backends consult. `min` counts the required positional params (no
// default, not kw-only/rest); `max` is the regular positional count;
// `variadic` is true when a `*args` catch-all removes the upper bound.
// A param carries a default when either a default expression (user
// source) or a literal default Value (C++-built stdlib entry) is set.
struct ArityBounds {
  long min;
  long max;
  bool variadic;
};
template <class P>
inline ArityBounds builtin_arity_bounds(const std::vector<P>& params) {
  long required = 0;
  for (const auto& p : params) {
    if (p.kw_only || p.kwargs_rest || p.args_rest) continue;
    if (p.default_expr == nullptr && p.default_value == nullptr) required++;
  }
  return {required, static_cast<long>(regular_param_count(params)),
          has_args_rest(params)};
}

// Built-in methods bind their arguments positionally — `'ab'.repeat(2)`, never
// `'ab'.repeat(n: 2)`. The one name a keyword argument may carry is a
// KEYWORD-ONLY parameter, which has no positional slot to bind through
// (`[3, 1].sorted(reverse: true)`). A `**` splat names nothing until run time,
// so a built-in never accepts one. Both backends gate on this, which is what
// keeps a built-in's keyword surface inside what the JIT's per-method codegen
// can bind statically.
template <class Ps>  // any range of parameters (vector / span)
inline bool builtin_method_accepts_keyword(const Ps& params,
                                           std::string_view name) {
  for (const auto& p : params)
    if (p.kw_only && p.name == name) return true;
  return false;
}

// Whether a callback declaring `cb_min` required and `cb_max` total regular
// positional params (cb_max < 0 means a `*args` catch-all removed the upper
// bound) can stand in for a higher-order callback invoked with exactly
// `expected` positional args. The single source of truth both backends
// consult (interp's `check_callback_arity`, the JIT's
// `_culebra_expect_callback`) so they accept and reject the same callbacks.
//
// A variadic callback absorbs any surplus, so it only needs its required
// params met (`expected >= cb_min`). A fixed-arity callback must be called
// with *exactly* its regular count: a HOF always supplies `expected` args, so
// accepting fewer would leave a defaulted param unfilled — and a callback's
// parameter defaults are arbitrary expressions only the full call binder can
// evaluate, which the per-element callback path deliberately doesn't run.
// Requiring `expected == cb_max` means every regular param is positionally
// filled, so no default ever needs evaluating and both backends behave
// identically (matching the JIT's historical exact-arity callback rule).
inline bool callback_arity_accepts(int64_t cb_min, int64_t cb_max,
                                   int64_t expected) {
  return cb_max < 0 ? expected >= cb_min : expected == cb_max;
}

// For a PARAMETER AST node, returns the DEFAULT_VALUE's inner expression
// (or nullptr if the parameter has no default). PARAMETER layout is
// `[MUTABLE, IDENTIFIER, (TYPE_ANNOTATION)?, (DEFAULT_VALUE)?]` and the
// optimizer unwraps DEFAULT_VALUE's single EXPRESSION child.
inline const peg::Ast* extract_default_expr(const peg::Ast& param_node) {
  using namespace peg::udl;
  for (size_t i = 2; i < param_node.nodes.size(); i++) {
    if (param_node.nodes[i]->tag == "DEFAULT_VALUE"_) {
      return param_node.nodes[i]->nodes[0].get();
    }
  }
  return nullptr;
}

// View of a PARAMETER AST node — see grammar:
//   PARAMETER <- KWARGS_REST / KW_ONLY_SEP
//              / MUTABLE _ IDENTIFIER (_ TYPE_ANNOTATION)? (_ '=' _ DEFAULT_VALUE)?
// Three forms collapse to one struct: KW_ONLY_SEP (`*`) carries only the
// separator flag, KWARGS_REST (`**name`) carries name on the node token
// itself, normal parameters carry `[MUTABLE, IDENTIFIER, ...]`. Callers
// that consume multiple fields read through the view; pure boolean
// predicates (`is_kw_only_sep` / `is_kwargs_rest`) remain available.
struct ParameterView {
  bool is_kwargs_rest;          // tag == KWARGS_REST
  bool is_args_rest;            // tag == ARGS_REST (`*name`)
  bool is_kw_only_sep;          // tag == KW_ONLY_SEP
  bool is_mut;                  // normal form: node[0].token == "mut"
  std::string_view name;        // KWARGS_REST/ARGS_REST: p.token; normal: IDENTIFIER child
  size_t name_line;
  size_t name_col;
  std::string_view type_annotation;   // normal form, "" when absent
  const peg::Ast* default_value;      // normal form, nullptr when absent
  const peg::Ast* pattern;            // destructuring param node, else nullptr
};

inline ParameterView view_parameter(const peg::Ast& p) {
  if (is_kw_only_sep(p)) {
    return ParameterView{false, false, true, false, {}, p.line, p.column,
                         {}, nullptr, nullptr};
  }
  if (is_pattern_param(p)) {
    return ParameterView{false, false, false, false, {}, p.line, p.column,
                         {}, nullptr, &p};
  }
  auto loc = extract_param_name_loc(p);
  if (is_kwargs_rest(p)) {
    return ParameterView{true, false, false, false, loc.name, loc.line,
                         loc.column, {}, nullptr, nullptr};
  }
  if (is_args_rest(p)) {
    return ParameterView{false, true, false, false, loc.name, loc.line,
                         loc.column, {}, nullptr, nullptr};
  }
  return ParameterView{
      false, false, false,
      p.nodes[0]->token == "mut",
      loc.name, loc.line, loc.column,
      extract_type_annotation(p, 2),
      extract_default_expr(p),
      nullptr,
  };
}

// For a FUNCTION AST node, returns the declared return type (or {}) and
// writes the body's node index to *body_idx.
inline std::string_view extract_return_type(const peg::Ast& fn_ast,
                                            size_t& body_idx) {
  using namespace peg::udl;
  if (fn_ast.nodes.size() == 3 &&
      fn_ast.nodes[1]->tag == "RETURN_TYPE"_) {
    body_idx = 2;
    return fn_ast.nodes[1]->token;
  }
  body_idx = 1;
  return {};
}

// View of a FUNCTION or LAMBDA AST node — see grammar:
//   FUNCTION <- fn _ PARAMETERS (_ RETURN_TYPE)? _ BLOCK
//   LAMBDA   <- LAMBDA_PARAMS _ (BLOCK / EXPRESSION)
// Layouts: FUNCTION size 2 `[PARAMETERS, BLOCK]` or size 3 with
//          `[PARAMETERS, RETURN_TYPE, BLOCK]`; LAMBDA always size 2
//          `[LAMBDA_PARAMS, BODY]` (no return type slot).
// One struct covers both: `return_type` is "" for LAMBDA. `body` is
// `std::shared_ptr<peg::Ast>` so callers can hand it directly to
// `make_function_value` (which retains ownership).
// Phase 3 (`yield` / generator) will grow this view with an
// `is_generator` flag once YIELD scanning lands; centralizing here
// keeps the lowering site count to one.
struct FunctionView {
  const peg::Ast* params;             // node[0]
  std::string_view return_type;       // "" for LAMBDA / no annotation
  std::shared_ptr<peg::Ast> body;     // node[body_idx]
};

inline FunctionView view_function(const peg::Ast& fn) {
  size_t body_idx = 1;
  auto rt = extract_return_type(fn, body_idx);
  return FunctionView{fn.nodes[0].get(), rt, fn.nodes[body_idx]};
}

inline FunctionView view_lambda(const peg::Ast& lam) {
  return FunctionView{lam.nodes[0].get(), {}, lam.nodes[1]};
}

// Canonical source position for a call error (DispatchError, etc.): the
// callee, not the argument list. A CALL node is `callee POSTFIX...`, so
// nodes[0] is the called expression. Both backends report call errors
// here so `f(1)` points at `f`, not at `(`. Localizing the child index in
// parser.h keeps a grammar tweak to the CALL shape from silently shifting
// every call error's column.
inline std::pair<size_t, size_t> call_callee_position(const peg::Ast& call) {
  const auto& callee = *call.nodes[0];
  return {callee.line, callee.column};
}

// Parse the built-in trait preamble (`trait Stringer`, `trait Eq`,
// `trait Comparable` + defaults). Lazily cached so subsequent calls
// reuse the same AST; the AST is process-lifetime so dependent
// modules' string_views aliasing the source stay valid.
inline std::shared_ptr<peg::Ast> parse_builtin_traits_preamble();

inline std::shared_ptr<peg::Ast> desugar_regex_literals(
    std::shared_ptr<peg::Ast> node);

// Shared by every desugar_* synthesizer below (make_regex_compile_call,
// make_postfix_if, ...): builds a token leaf / group node with the
// `tag`/`original_tag` split the AstOptimizer emits for a hand-written node.
// Both backends resolve a call by each child's *original_tag* (PRIMARY
// callee / DOT method / ARGUMENTS list / ARG_ITEM args), not just by name —
// the single-arg `Node(name, token)` / `Node(name, nodes)` constructors set
// `original_tag == tag`, which the JIT's namespace resolution rejects. The
// copy-with-original_name constructor reproduces the real shape: `tok`/`grp`
// build a node whose `tag` comes from `name` and whose `original_tag` comes
// from `orig`, exactly as the optimizer emits for a hand-written call. It
// takes its source by const&, so a stack temporary is enough — no need to
// heap-allocate the inner node.
struct SynthNode {
  const char* path;
  size_t ln, col;

  std::shared_ptr<peg::Ast> tok(const char* name, const char* orig,
                                std::string_view t, size_t l, size_t c) const {
    return std::make_shared<peg::Ast>(peg::Ast(path, l, c, name, t), orig);
  }
  // `position`/`length` default to 0 (cpp-peglib's own default) for callers
  // that never need a source-text slice of the synthesized node. Passing the
  // real span matters for consumers that read raw source by offset instead
  // of walking the tree — e.g. generator_transform.h's CPS builder takes a
  // verbatim `ast_source_slice` of any statement it doesn't need to split,
  // which silently slices empty text from a position-less node.
  std::shared_ptr<peg::Ast> grp(
      const char* name, const char* orig,
      std::vector<std::shared_ptr<peg::Ast>> kids, size_t position = 0,
      size_t length = 0) const {
    // The relabeling copy-constructor below (AstBase(const AstBase&,
    // original_name, position, length, ...)) has its own position/length
    // params, defaulted to 0 independently of the temporary's — passing
    // `position`/`length` only to the inner peg::Ast(...) and not here would
    // silently zero them right back out.
    return std::make_shared<peg::Ast>(
        peg::Ast(path, ln, col, name, std::move(kids), position, length),
        orig, position, length);
  }
};

// A `re'...'` / `re"..."` / `` re`...` `` literal is desugared, after AST
// optimization, into an ordinary `Regex.compile(<pattern>, <flags>)` call so
// both backends reuse the existing Regex machinery (no new eval/codegen paths).
// The REGEX_LIT node carries two children: a REGEX_BODY (a sequence of raw
// REGEX_CONTENT tokens and `${expr}` REGEX_INTERP nodes) and a REGEX_FLAGS
// token ("" when absent).
//
// `<pattern>` is built from the body parts:
//   * no interpolation  -> a single raw STRING (byte-identical to the original,
//                          interpolation-free desugar).
//   * with interpolation -> a `+`-concatenation whose operands are raw STRINGs
//                          (literal runs) and `Regex.interp(expr)` calls (one
//                          per `${expr}`). `Regex.interp` escapes a String so
//                          it matches literally and splices a compiled Regex as
//                          a non-capturing group — injection-safe by default.
inline std::shared_ptr<peg::Ast> make_regex_compile_call(const peg::Ast& lit) {
  using Node = peg::Ast;
  const char* path = lit.path.c_str();
  size_t ln = lit.line, col = lit.column;
  const auto& body = *lit.nodes[0];   // REGEX_BODY
  const auto& flags = *lit.nodes[1];  // REGEX_FLAGS

  SynthNode nf{path, ln, col};
  auto tok = [&](const char* name, const char* orig, std::string_view t,
                 size_t l, size_t c) { return nf.tok(name, orig, t, l, c); };
  auto grp = [&](const char* name, const char* orig,
                 std::vector<std::shared_ptr<Node>> kids) {
    return nf.grp(name, orig, std::move(kids));
  };
  // Relabel an existing node's `original_tag` (here, to ARG_ITEM) while keeping
  // its tag and children, matching the shape the optimizer emits for a
  // hand-written call argument.
  auto as_arg = [&](const std::shared_ptr<Node>& n) {
    return std::make_shared<Node>(Node(*n, "ARG_ITEM"));
  };

  // `Regex.interp(<expr>)` — wraps an interpolated expression. Nested regex
  // literals inside `${...}` are desugared first (the outer walk does not
  // recurse into a call it just synthesized).
  auto make_interp_call = [&](const peg::Ast& interp) {
    std::shared_ptr<Node> expr = desugar_regex_literals(interp.nodes[0]);
    std::vector<std::shared_ptr<Node>> a;
    a.push_back(as_arg(expr));
    std::vector<std::shared_ptr<Node>> c;
    c.push_back(
        tok("IDENTIFIER", "PRIMARY", std::string_view("Regex"), ln, col));
    c.push_back(tok("IDENTIFIER", "DOT", std::string_view("interp"), ln, col));
    c.push_back(grp("ARG_LIST", "ARGUMENTS", std::move(a)));
    return grp("CALL", "CALL", std::move(c));
  };

  using namespace peg::udl;

  // Build the pattern operands in source order: a raw STRING per REGEX_CONTENT
  // run, a `Regex.interp(expr)` call per REGEX_INTERP.
  std::vector<std::shared_ptr<Node>> operands;
  bool has_interp = false;
  for (const auto& part : body.nodes) {
    if (part->tag == "REGEX_INTERP"_) {
      has_interp = true;
      operands.push_back(make_interp_call(*part));
    } else {  // REGEX_CONTENT
      operands.push_back(
          tok("STRING", "STRING", part->token, part->line, part->column));
    }
  }

  std::shared_ptr<Node> pattern;
  if (!has_interp) {
    // Single raw STRING (empty body -> ""). Identical to the original desugar.
    std::string_view text = operands.empty() ? std::string_view("")
                                             : operands.front()->token;
    pattern = tok("STRING", "ARG_ITEM", text, body.line, body.column);
  } else if (operands.size() == 1) {
    pattern = as_arg(operands.front());  // lone `${expr}` -> just the call
  } else {
    // Interleave operands with `+` into a left-associative ADDITIVE chain:
    // [op0, '+', op1, '+', op2, ...]. eval_bin_expression / compile_additive
    // both read children as (operand, operator, operand, ...).
    std::vector<std::shared_ptr<Node>> chain;
    chain.push_back(operands.front());
    for (size_t i = 1; i < operands.size(); ++i) {
      chain.push_back(tok("ADDITIVE_OPERATOR", "ADDITIVE_OPERATOR",
                          std::string_view("+"), ln, col));
      chain.push_back(operands[i]);
    }
    pattern = grp("ADDITIVE", "ARG_ITEM", std::move(chain));
  }

  std::vector<std::shared_ptr<Node>> args;
  args.push_back(std::move(pattern));
  if (!flags.token.empty()) {
    args.push_back(
        tok("STRING", "ARG_ITEM", flags.token, flags.line, flags.column));
  }

  std::vector<std::shared_ptr<Node>> call;
  call.push_back(tok("IDENTIFIER", "PRIMARY", std::string_view("Regex"), ln, col));
  call.push_back(tok("IDENTIFIER", "DOT", std::string_view("compile"), ln, col));
  call.push_back(grp("ARG_LIST", "ARGUMENTS", std::move(args)));
  return grp("CALL", "CALL", std::move(call));
}

// Walk the optimized AST, replacing every REGEX_LIT with its desugared call.
// The tree is freshly built by the optimizer and owned solely by the caller,
// so children are rewritten in place.
inline std::shared_ptr<peg::Ast> desugar_regex_literals(
    std::shared_ptr<peg::Ast> node) {
  using namespace peg::udl;
  // Match on `tag` (the node's own name): the optimizer may collapse a
  // single-child EXPRESSION onto the REGEX_LIT, which rewrites `original_tag`
  // to EXPRESSION while `tag` stays REGEX_LIT.
  if (node->tag == "REGEX_LIT"_) return make_regex_compile_call(*node);
  for (auto& child : node->nodes) child = desugar_regex_literals(child);
  return node;
}

inline std::shared_ptr<peg::Ast> desugar_postfix_modifiers(
    std::shared_ptr<peg::Ast> node);

// `stmt if cond` / `stmt unless cond` (see view_postfix_modifier) is
// desugared, after AST optimization, into an ordinary IF node — `[cond,
// then]` for `if`, `[!cond, then]` for `unless` — so both backends reuse
// eval_if / compile_if unchanged; no new eval/codegen path. `unless`'s
// negation is a synthesized UNARY_NOT, matching how a hand-written `!cond`
// parses. `then` is a relabeled copy of the base statement whose
// original_tag is set to STATEMENT — deliberately different from the BLOCK
// tag a hand-written `{ stmt }` body gets, so `then` fires the debugger's
// statement hook via _eval_dispatch's unconditional `original_tag ==
// STATEMENT` check instead of the if-arm's
// is_collapsed_single_statement fallback, the same way any ordinary
// top-level statement does.
inline std::shared_ptr<peg::Ast> make_postfix_if(const peg::Ast& stmt) {
  using Node = peg::Ast;
  const char* path = stmt.path.c_str();
  size_t ln = stmt.line, col = stmt.column;
  bool is_unless = view_postfix_modifier(stmt).is_unless;
  SynthNode nf{path, ln, col};
  auto tok = [&](const char* name, const char* orig, std::string_view t,
                size_t l, size_t c) { return nf.tok(name, orig, t, l, c); };
  auto grp = [&](const char* name, const char* orig,
                std::vector<std::shared_ptr<Node>> kids, size_t position = 0,
                size_t length = 0) {
    return nf.grp(name, orig, std::move(kids), position, length);
  };

  std::shared_ptr<Node> cond =
      desugar_postfix_modifiers(stmt.nodes[1]->nodes[0]);
  std::shared_ptr<Node> base = desugar_postfix_modifiers(stmt.nodes[0]);
  // Real position/length (not the SynthNode default of 0), so a consumer
  // that reads raw source by offset instead of walking the tree — e.g.
  // generator_transform.h's CPS builder, which takes a verbatim
  // `ast_source_slice` of any statement it doesn't need to split — slices
  // `then`'s own source text instead of an empty string.
  std::shared_ptr<Node> then_body = std::make_shared<Node>(
      Node(*base, "STATEMENT", base->position, base->length));

  if (is_unless) {
    auto bang = tok("UNARY_NOT_OPERATOR", "UNARY_NOT_OPERATOR",
                    std::string_view("!"), cond->line, cond->column);
    cond = grp("UNARY_NOT", "UNARY_NOT", {bang, cond});
  }

  // The whole IF node's span mirrors `stmt`'s (start of the base statement
  // through the end of the condition) — source order is reversed from the
  // synthesized tree shape, but the slice is still the original, valid
  // `stmt if cond` / `stmt unless cond` text, byte for byte.
  return grp("IF", "IF", {cond, then_body}, stmt.position, stmt.length);
}

// Walk the optimized AST, replacing every postfix-modifier STATEMENT with
// its desugared IF (see make_postfix_if). Mirrors desugar_regex_literals.
inline std::shared_ptr<peg::Ast> desugar_postfix_modifiers(
    std::shared_ptr<peg::Ast> node) {
  if (has_postfix_modifier(*node)) return make_postfix_if(*node);
  for (auto& child : node->nodes) child = desugar_postfix_modifiers(child);
  return node;
}

// Rules the AstOptimizer must NOT collapse onto a single child — the shared
// single source of truth for every consumer of the optimized AST (interp /
// JIT / lint / fmt). Adding a structural node to the grammar usually means
// adding its name here too.
inline const std::vector<std::string>& ast_optimizer_keep_rules() {
  static const std::vector<std::string> rules = {
      "PARAMETERS", "LAMBDA_PARAMS", "SEQUENCE", "OBJECT",
      "OBJECT_PROPERTY", "TUPLE", "SET",
      "ARRAY", "RETURN",
      "THROW", "YIELD", "YIELD_FROM", "TRY", "DEFER", "FOR",
      "LEXICAL_SCOPE", "TYPE_ANNOTATION", "RETURN_TYPE",
      "DEFAULT_VALUE",
      "ARG_LIST", "KWARG", "KWARG_SPLAT",
      "CLASS_DECL", "METHOD", "DECORATOR",
      "TRAIT_DECL", "TRAIT_METHOD", "TRAIT_BODY",
      "ENUM_DECL", "VARIANT",
      "EFFECT_FN_DECL", "PERFORM", "HANDLE", "HANDLE_CLAUSE", "RETURN_CLAUSE",
      // TUPLE_PATTERN must be kept: the single-element form `(a,)`
      // (grammar's `'(' PATTERN ',' ')'` alt) has exactly one child and
      // would otherwise collapse onto its lone sub-pattern, losing the
      // tuple tag. FOR_BINDING is deliberately NOT kept: it emits its own
      // name and relies on the same single-child collapse for `for x in`
      // (see grammar_def.h). The two used to share the TUPLE_PATTERN name
      // — that collision is what made `(a,)` uncollapsible-yet-collapsed.
      "TUPLE_PATTERN",
      // INIT_CLAUSE (the optional `while` init clause) must be kept: a single
      // binding would otherwise collapse onto its lone ASSIGNMENT, and the
      // backends detect the init clause by WHILE's first child being an
      // INIT_CLAUSE node (see view_while). INIT_BINDING is deliberately NOT
      // kept — it is a single-alternative wrapper that collapses to its
      // ASSIGNMENT / DESTRUCTURE_ASSIGN, which is what the walkers expect.
      "INIT_CLAUSE",
      // NOBREAK_CLAUSE (`while/for … nobreak { … }`) wraps a single BLOCK; kept
      // so the loop's last child is a NOBREAK_CLAUSE tag when present (else it
      // would collapse onto the BLOCK and be indistinguishable from the body).
      "NOBREAK_CLAUSE",
      // BREAK / CONTINUE must be kept: a labelled `break outer` has exactly
      // one child (its LOOP_LABEL) and would otherwise collapse onto the
      // label, losing the statement's own tag. The unlabelled form is a
      // childless leaf either way.
      "BREAK", "CONTINUE",
      "MATCH_ARMS", "GUARD", "COND", "COND_ARM",
      "ARRAY_PATTERN", "OBJECT_PATTERN",
      "CTOR_PATTERN",
      "REST_PATTERN", "INTERP_EXPR", "INTERPOLATED_STRING",
      // FORMAT_SPEC wraps its chunks, and SPEC_ARG a single EXPRESSION; both
      // would collapse onto that one child when a spec has exactly one piece
      // (`{x:.2f}` / `{s:>{w}}` minus the literal), and the pieces would then
      // be indistinguishable from each other. See view_interp_expr.
      "FORMAT_SPEC", "SPEC_ARG",
      "TRIPLE_STRING", "REGEX_LIT", "REGEX_BODY", "REGEX_INTERP",
      "SPREAD_ELEM",
      "IMPORT_STMT", "EXPORT_STMT",
      // STMT_MODIFIER_IF/_UNLESS (`stmt if cond` / `stmt unless cond`) each
      // wrap a single condition EXPRESSION; kept so a lone modifier condition
      // does not collapse and lose the tag has_postfix_modifier / the
      // formatter's printer key on (see view_postfix_modifier).
      "STMT_MODIFIER_IF", "STMT_MODIFIER_UNLESS",
      // BY_STEP (`0..10 by 2`) wraps a single ADDITIVE step expression; kept
      // so RANGE's decoder can tell a step clause apart from an end bound
      // positionally (see grammar_def.h).
      "BY_STEP"};
  return rules;
}

// `expr` is taken by mutable reference because the newline normalization edits
// it: the AST's tokens are string_views into this buffer, so it is the one that
// has to hold the normalized bytes. Callers already had to keep it alive for as
// long as the AST — they now also have to own it rather than share it, which is
// only visible to a caller parsing one buffer from several threads.
inline std::shared_ptr<peg::Ast> parse(const std::string& path,
                                       std::string& expr,
                                       std::vector<std::string>& msgs) {
  auto& parser = get_parser();
  _culebra_parse_depth = 0;  // an aborted parse leaves the count mid-flight

  parser.set_logger([&](size_t ln, size_t col, const std::string& err_msg) {
    msgs.push_back(std::format("{}:{}:{}: {}\n", path, ln, col, err_msg));
  });

  std::shared_ptr<peg::Ast> ast;
  // The depth guard throws from peglib's enter hook, so it bypasses the
  // logger. Convert it here so every caller sees one failure shape
  // (msgs + nullptr) — before this, each caller needed its own catch, and
  // fmt/lint/doctest/repl/dap each missed it in turn. The newline
  // normalization below reports a bare `\r` the same way.
  try {
    normalize_source_newlines(expr);
    if (!parser.parse_n(expr.data(), expr.size(), ast, path.c_str())) {
      return nullptr;
    }
  } catch (const CulebraError& e) {
    msgs.push_back(std::format("{}:{}:{}: {}\n", path, e.line, e.col, e.what()));
    return nullptr;
  }

  auto opt = peg::AstOptimizer(true, ast_optimizer_keep_rules());
  return desugar_postfix_modifiers(desugar_regex_literals(opt.optimize(ast)));
}

// Like parse(), but keeps REGEX_LIT and postfix-modifier (`stmt if cond` /
// `stmt unless cond`) nodes intact — no desugaring to a `Regex.compile(...)`
// call or a plain IF — so the source formatter can re-emit both forms
// verbatim. Used only by `culebra fmt`; never feed this AST to a backend,
// which expects the desugared form.
inline std::shared_ptr<peg::Ast> parse_for_format(
    const std::string& path, std::string& expr,
    std::vector<std::string>& msgs) {
  auto& parser = get_parser();
  _culebra_parse_depth = 0;  // an aborted parse leaves the count mid-flight

  parser.set_logger([&](size_t ln, size_t col, const std::string& err_msg) {
    msgs.push_back(std::format("{}:{}:{}: {}\n", path, ln, col, err_msg));
  });

  std::shared_ptr<peg::Ast> ast;
  try {  // depth guard + bare `\r` → msgs, as in parse()
    normalize_source_newlines(expr);
    if (!parser.parse_n(expr.data(), expr.size(), ast, path.c_str())) {
      return nullptr;
    }
  } catch (const CulebraError& e) {
    msgs.push_back(std::format("{}:{}:{}: {}\n", path, e.line, e.col, e.what()));
    return nullptr;
  }

  auto opt = peg::AstOptimizer(true, ast_optimizer_keep_rules());
  return opt.optimize(ast);
}

inline std::shared_ptr<peg::Ast> parse_builtin_traits_preamble() {
  // `cached`'s AST holds string_view tokens into this buffer, so it must
  // outlive `cached` itself — static, not a lambda-local temporary.
  static std::string src{builtin_traits_preamble()};
  static auto cached = [] {
    std::vector<std::string> ignore;
    return parse("<builtin>", src, ignore);
  }();
  return cached;
}

// Invoke `f(name, line, column)` for each identifier that `pattern`
// would bind on a successful match. `_` (sink) is skipped. A pure AST
// helper (no runtime types), shared by the interpreter, the JIT, and the
// static lint pass.
template <typename F>
inline void for_each_pattern_binding(const peg::Ast& pattern, F&& f) {
  using namespace peg::udl;
  if (pattern.tag == "PATTERN"_ && !pattern.nodes.empty()) {
    for (auto& sub : pattern.nodes) for_each_pattern_binding(*sub, f);
    return;
  }
  auto emit = [&](std::string_view name, size_t line, size_t col) {
    if (name != "_") f(name, line, col);
  };
  switch (pattern.tag) {
    case "IDENTIFIER"_:
      emit(pattern.token, pattern.line, pattern.column);
      return;
    case "TYPED_IDENT"_: {
      auto& id = *pattern.nodes[0];
      emit(id.token, id.line, id.column);
      return;
    }
    case "ARRAY_PATTERN"_:
    case "TUPLE_PATTERN"_:
    case "FOR_BINDING"_:  // multi-target `for k, v in …` — same shape as a tuple
      for (auto& e : pattern.nodes) for_each_pattern_binding(*e, f);
      return;
    case "CTOR_PATTERN"_:
      // nodes[0] = CTOR_PATH (the ctor name token); nodes[1..] are the
      // positional payload sub-patterns, each of which may bind.
      for (size_t i = 1; i < pattern.nodes.size(); i++) {
        for_each_pattern_binding(*pattern.nodes[i], f);
      }
      return;
    case "REST_PATTERN"_: {
      auto& id = *pattern.nodes[0];
      emit(id.token, id.line, id.column);
      return;
    }
    case "OBJECT_PATTERN"_:
      for (auto& entry : pattern.nodes) {
        // `OBJECT_PAT_ENTRY` carries [IDENTIFIER, PATTERN]; recurse
        // into the sub-pattern. Bare IDENTIFIER is the shorthand form
        // — bind the key as the slot name.
        if (entry->tag == "OBJECT_PAT_ENTRY"_) {
          for_each_pattern_binding(*entry->nodes[1], f);
        } else {
          emit(entry->token, entry->line, entry->column);
        }
      }
      return;
    default:
      return;
  }
}

}  // namespace culebra
