// Generator (yield) AST transformation pass.
//
// A `fn` whose body contains `yield` / `yield from` is rewritten, at
// parse time, into an anonymous class implementing the Iterator
// protocol (iter / has_next / next / dispose), then re-parsed — the
// interp / JIT / AOT runtimes execute the synthesized class with no
// generator-specific support of their own.
//
//   fn name(...) { ... yield ... }
//     ->
//   fn name(...) {
//     class _Gen_<name>_<line>_<col> { new(...) {...} iter()/has_next()/
//                                      next()/dispose() }
//     _Gen_<name>_<line>_<col>.new(...)
//   }
//
// The body is lowered by a flat-dispatch CPS state machine (see
// `CpsBuilder`): each basic block becomes a state, control flow becomes
// `this._g_state = K; continue` jumps over one `while true` dispatch
// loop, and every local lives on the instance (all-locals-on-heap, so no
// liveness analysis). Two source-level pre-passes run first: the C# rule
// rejects yield inside try-catch/defer, and yielding for-in loops are
// desugared to `while it.has_next()` form. See
// [[project-generator-design]] for the design axes and the frozen spec.

#pragma once

#include "parser.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace culebra {

// Storage for synthesized generator source fragments. peg::Ast holds
// `string_view`s into the parsed source, so anything the transform
// re-parses needs process-lifetime backing. See
// [[feedback-ast-source-lifetime]] — same fix the lazy-module path
// uses.
inline std::vector<std::shared_ptr<std::string>>&
generator_transform_sources() {
  static std::vector<std::shared_ptr<std::string>> sources;
  return sources;
}

// These tag types open a fresh fn-body scope: anything found inside (a
// yield, a local, a class decl) belongs to the *inner* function, not the
// enclosing one — walkers stop here so it isn't reattributed upward.
inline bool is_fn_boundary(unsigned int tag) {
  using namespace peg::udl;
  return tag == "FUNCTION"_ || tag == "LAMBDA"_ || tag == "MULTIFN_DECL"_;
}

// First YIELD or YIELD_FROM belonging to this fn body, stopping at fn
// boundaries (see `is_fn_boundary`) — a nested generator's yields are its
// own. Treating the two as equivalent here lets Stage 5's for-in desugar
// fire even when the only yield-shaped node inside is a `yield from`.
// nullptr when absent.
inline const peg::Ast* find_yield_in_fn_body(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "YIELD"_ || node.tag == "YIELD_FROM"_) return &node;
  if (is_fn_boundary(node.tag)) return nullptr;
  for (auto& c : node.nodes) {
    if (auto* y = find_yield_in_fn_body(*c)) return y;
  }
  return nullptr;
}

inline bool fn_body_has_yield(const peg::Ast& node) {
  return find_yield_in_fn_body(node) != nullptr;
}

// Collect every DEFER node in a fn body in source order, stopping at
// inner fn boundaries. Used by the dispatcher to verify that all defers
// live at the body's top level (where Stage 3 emits them into the
// generator's defer registry); defers buried inside loop/if bodies have
// no good translation today and surface as a SyntaxError.
inline std::vector<const peg::Ast*> collect_defers(const peg::Ast& body) {
  using namespace peg::udl;
  std::vector<const peg::Ast*> out;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    if (is_fn_boundary(n.tag)) return;
    if (n.tag == "DEFER"_) { out.push_back(&n); return; }
    for (auto& c : n.nodes) walk(*c);
  };
  walk(body);
  return out;
}

// Locate the first YIELD reachable from inside a TRY's try-block / catch
// block, or from inside a DEFER's body. Returns nullptr if no such yield
// exists. Used by the dispatcher to enforce the C# rule (CS1626): yield
// statements may not appear inside a try-catch or defer. `yield try {...}
// catch e {...}` is fine because the yield is OUTSIDE the try (only the
// try-expression's value flows through it) — the guard descends into a
// TRY's body BLOCKs but not into its position as an expression.
inline const peg::Ast* find_yield_inside_try_or_defer(const peg::Ast& body) {
  using namespace peg::udl;
  const peg::Ast* found = nullptr;
  std::function<void(const peg::Ast&, bool)> walk =
      [&](const peg::Ast& n, bool inside_guard) {
        if (found) return;
        if (is_fn_boundary(n.tag)) return;
        if (inside_guard && (n.tag == "YIELD"_ || n.tag == "YIELD_FROM"_)) {
          found = &n;
          return;
        }
        if (n.tag == "TRY"_) {
          // TRY children: try-BLOCK, catch-IDENT, catch-BLOCK
          if (n.nodes.size() > 0) walk(*n.nodes[0], true);
          if (n.nodes.size() > 2) walk(*n.nodes[2], true);
          return;
        }
        if (n.tag == "DEFER"_) {
          if (!n.nodes.empty()) walk(*n.nodes[0], true);
          return;
        }
        for (auto& c : n.nodes) walk(*c, inside_guard);
      };
  walk(body, false);
  return found;
}

// Locate the first named function definition (`fn name(...) { ... }`, a
// MULTIFN_DECL) that appears as a statement inside a generator body — at the
// top level or nested in its control flow (if / for / while / block / try),
// but NOT inside a nested fn VALUE (an anonymous `fn (...) {...}` / `|x| ...`,
// which opens its own scope and binds fine). Returns nullptr if none. Used to
// reject the construct uniformly: the JIT's CPS lowering doesn't bind such a
// definition in the generator's state frame, so interp and JIT diverge.
inline const peg::Ast* find_nested_fndef(const peg::Ast& body) {
  using namespace peg::udl;
  const peg::Ast* found = nullptr;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    for (auto& c : n.nodes) {
      if (found) return;
      if (c->tag == "MULTIFN_DECL"_) {
        found = c.get();
        return;
      }
      // A nested fn VALUE keeps its own scope, and a nested `handle` opens
      // its own computation scope (the effects pass validates fns inside it)
      // — leave both (and their inner defs) alone.
      if (c->tag == "FUNCTION"_ || c->tag == "LAMBDA"_ || c->tag == "HANDLE"_)
        continue;
      walk(*c);
    }
  };
  walk(body);
  return found;
}

// Slice the source range an AST node spans. peg::Ast carries
// `position` + `length` in bytes; safe so long as `src` is the same
// buffer that produced `node`.
inline std::string_view ast_source_slice(const peg::Ast& node,
                                          const char* src, size_t src_len) {
  if (node.position + node.length > src_len) return {};
  return std::string_view(src + node.position, node.length);
}

// --- Source-text helpers -------------------------------------------------

// Drop the enclosing `{` / `}` from a BLOCK source slice. A BLOCK that's
// been parsed will always be wrapped in braces, but the check stays
// defensive in case the slice is already brace-stripped or empty.
inline std::string_view strip_block_braces(std::string_view s) {
  if (s.size() >= 2 && s.front() == '{' && s.back() == '}') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

// Rewrite every standalone `<name>` in `src` to `this.<name>` for each
// name in `names`, then strip `let `/`mut ` prefixes that now sit in
// front of `this.X` (assignment, no longer declaration).
//
// The match requires the identifier NOT be preceded by `.` — so a
// member access like `arr.size()` is left alone even when a local/param
// is named `size`. Without this, `\bsize\b` rewrote the `.size()` method
// name to `arr.this.size()`, producing malformed source (a parser crash
// for any generator whose binding collides with a builtin method like
// size / push / keys). The same `.`-exclusion also prevents re-rewriting
// the `this.` prefixes this pass just inserted. The `..` range operator is
// exempted from that exclusion: `0..n`'s `n` is an operand, not a member,
// so a range bound naming a local/param (`for i in 0..n`, desugared to
// `(0..n).iter()`) must still be rewritten to `0..this.n`. String-literal
// content and comments (line `#`/`//`, block `/* … */`) are skipped by
// `rewrite_outside_strings` below, so a local name appearing there as literal
// text is never rewritten.
// `"""` starts at `i`.
inline bool is_triple_quote(std::string_view s, size_t i) {
  return i + 2 < s.size() && s[i] == '"' && s[i + 1] == '"' && s[i + 2] == '"';
}

// Index just past the string literal starting at `i` (`'…'` / `` `…` `` raw,
// `"…"` / `"""…"""` interpolated). Escapes (`\`) are honoured in the
// double-quoted forms; the raw forms end at the first closing delimiter.
inline size_t skip_string_literal(std::string_view s, size_t i) {
  size_t n = s.size();
  char q = s[i];
  if (q == '\'' || q == '`') {
    for (i++; i < n && s[i] != q; i++) {}
    return i < n ? i + 1 : i;
  }
  bool triple = is_triple_quote(s, i);
  i += triple ? 3 : 1;
  while (i < n) {
    if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
    if (triple) {
      if (is_triple_quote(s, i)) return i + 3;
    } else if (s[i] == '"') {
      return i + 1;
    }
    i++;
  }
  return i;
}

// Apply `rewrite` to `src`, but only to its *code* — the literal content of
// string constants and comments (line `#`/`//`, block `/* … */`) is copied
// verbatim so an identifier that happens to appear as literal text is never
// touched. Interpolation `{expr}` inside a `"…"` / `"""…"""` string IS code and
// is rewritten (recursively, as an expr may embed further strings). Only the
// double-quoted forms interpolate; `'…'` / `` `…` `` are fully raw.
inline std::string rewrite_outside_strings(
    std::string_view s,
    const std::function<std::string(std::string_view)>& rewrite) {
  std::string out;
  size_t i = 0, n = s.size(), code0 = 0;
  auto flush = [&](size_t end) {
    if (end > code0) out += rewrite(s.substr(code0, end - code0));
  };
  auto copy_verbatim = [&](size_t j) {
    out.append(s.substr(i, j - i));
    i = code0 = j;
  };
  while (i < n) {
    char c = s[i];
    bool line_comment = c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/');
    if (line_comment) {
      flush(i);
      size_t j = i;
      while (j < n && s[j] != '\n') j++;
      copy_verbatim(j);
    } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {  // block comment
      flush(i);
      size_t j = i + 2;
      while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
      copy_verbatim(j + 1 < n ? j + 2 : n);
    } else if (c == '\'' || c == '`') {  // raw string, no interpolation
      flush(i);
      copy_verbatim(skip_string_literal(s, i));
    } else if (c == '"') {  // interpolated (or triple) string
      flush(i);
      bool triple = is_triple_quote(s, i);
      size_t open = triple ? 3 : 1;
      out.append(s.substr(i, open));
      i += open;
      while (i < n) {
        if (!triple && s[i] == '"') { out += '"'; i++; break; }
        if (triple && is_triple_quote(s, i)) { out.append("\"\"\""); i += 3; break; }
        if (s[i] == '\\' && i + 1 < n) { out.append(s.substr(i, 2)); i += 2; continue; }
        if (s[i] == '{') {  // interpolation expr — code, may embed strings
          size_t k = i + 1, depth = 1;
          while (k < n && depth > 0) {
            char d = s[k];
            if (d == '{') { depth++; k++; }
            else if (d == '}') { if (--depth == 0) break; k++; }
            else if (d == '"' || d == '\'' || d == '`') k = skip_string_literal(s, k);
            else k++;
          }
          out += '{';
          out += rewrite_outside_strings(s.substr(i + 1, k - (i + 1)), rewrite);
          if (k < n) { out += '}'; k++; }
          i = k;
        } else { out += s[i]; i++; }
      }
      code0 = i;
    } else {
      i++;
    }
  }
  flush(n);
  return out;
}

// --- line-provenance markers ---------------------------------------------
//
// The generator / effects transforms rewrite body SOURCE TEXT and re-parse it,
// so the re-parsed AST's line numbers are fragment-relative and every error
// reported through them used to point nowhere near the user's code. The fix is
// a `#line`-style provenance marker: each user code line gets a trailing
// ` #@culebra:<original-line>` comment when the body is first sliced out of
// the real file. Being a comment, the marker survives every later text stage (for-in
// desugar, ANF hoisting, CPS state emission, nested re-lowering) for free —
// verbatim slices carry it, synthesized lines simply lack one. After the FINAL
// fragment parse, `marker_line_map` reads the markers back and
// `reposition_ast` rebuilds the AST with original line numbers (columns stay
// fragment-relative — approximate, since `this.` insertion shifts them).
// Synthesized machinery lines fall back to the construct's declaration line.

// The marker token. Deliberately verbose so a user comment ending in a bare
// `#@123` can't be mistaken for provenance.
inline constexpr std::string_view kLineMarker = "#@culebra:";

// The ` #@culebra:N` suffix for a line whose original line is `n` — the one
// writer of the format `line_has_marker` / `marker_line_map` read back.
inline std::string line_marker(long n) {
  return std::format(" {}{}", kLineMarker, n);
}

// 1-based line number of byte offset `pos` in `s`.
inline long line_of_offset(std::string_view s, size_t pos) {
  long line = 1;
  for (size_t i = 0; i < pos && i < s.size(); i++) {
    if (s[i] == '\n') line++;
  }
  return line;
}

// For each line of `s` (1-based; index 0 unused), whether its END (the `\n`,
// or EOF for the last line) sits in code / comment context — i.e. a trailing
// `#@culebra:N` marker comment may be placed or read there. A line ending inside a
// string literal (multi-line triple string) or an interpolation hole is
// unsafe. This walk mirrors `rewrite_outside_strings`' lane logic at line
// granularity; the two are kept as documented twins (unifying them under one
// lane scanner is deferred until a third consumer appears).
inline std::vector<bool> safe_line_ends(std::string_view s) {
  std::vector<bool> safe;
  safe.push_back(false);  // index 0 unused
  size_t i = 0, n = s.size();
  auto endline = [&](bool ok) { safe.push_back(ok); };
  while (i < n) {
    char c = s[i];
    if (c == '\n') { endline(true); i++; continue; }
    if (c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/')) {
      while (i < n && s[i] != '\n') i++;   // comment tail: its EOL is safe
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {  // block comment
      i += 2;
      while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
        if (s[i] == '\n') endline(true);   // marker inside a comment is fine
        i++;
      }
      i = (i + 1 < n) ? i + 2 : n;
      continue;
    }
    if (c == '\'' || c == '`') {  // raw string: interior EOLs unsafe
      size_t j = skip_string_literal(s, i);
      for (size_t k = i; k < j; k++) {
        if (s[k] == '\n') endline(false);
      }
      i = j;
      continue;
    }
    if (c == '"') {  // interpolated / triple string; holes conservatively unsafe
      size_t j = skip_string_literal(s, i);
      for (size_t k = i; k < j; k++) {
        if (s[k] == '\n') endline(false);
      }
      i = j;
      continue;
    }
    i++;
  }
  endline(true);  // last line (no trailing newline): EOF in code context
  return safe;
}

// True when the line already carries a trailing `#@culebra:N` marker.
inline bool line_has_marker(std::string_view line) {
  size_t e = line.size();
  size_t d = e;
  while (d > 0 && std::isdigit(static_cast<unsigned char>(line[d - 1]))) d--;
  return d < e && d >= kLineMarker.size() &&
         line.substr(d - kLineMarker.size(), kLineMarker.size()) == kLineMarker;
}

// True when any marker-safe line of `text` carries a trailing marker — i.e.
// the text is an already-annotated fragment. A marker-shaped substring inside
// a string literal doesn't count (its line end is unsafe).
inline bool text_has_line_marker(std::string_view text) {
  auto safe = safe_line_ends(text);
  size_t start = 0, idx = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    size_t end = (nl == std::string_view::npos) ? text.size() : nl;
    if ((idx + 1) < safe.size() && safe[idx + 1] &&
        line_has_marker(text.substr(start, end - start))) {
      return true;
    }
    if (nl == std::string_view::npos) break;
    start = nl + 1;
    idx++;
  }
  return false;
}

// Append ` #@culebra:<first_line + i>` to each marker-safe line of `text` — the
// entry-point annotation for a body sliced verbatim out of the ORIGINAL file
// (so line numbering is contiguous). Lines that already carry a marker (a body
// re-sliced from an annotated fragment never reaches here, but a user comment
// could imitate one) keep the existing value.
inline std::string annotate_line_markers(std::string_view text,
                                         long first_line) {
  auto safe = safe_line_ends(text);
  std::string out;
  size_t start = 0, idx = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    size_t end = (nl == std::string_view::npos) ? text.size() : nl;
    std::string_view line = text.substr(start, end - start);
    out += line;
    bool ok = (idx + 1) < safe.size() && safe[idx + 1];
    if (ok && !line.empty() && !line_has_marker(line)) {
      out += line_marker(first_line + static_cast<long>(idx));
    }
    if (nl == std::string_view::npos) break;
    out += '\n';
    start = nl + 1;
    idx++;
  }
  return out;
}

// Read the markers back: for each 1-based line of `text`, its original line,
// or 0 when the line carries none (synthesized machinery).
inline std::vector<long> marker_line_map(std::string_view text) {
  auto safe = safe_line_ends(text);
  std::vector<long> map;
  map.push_back(0);  // index 0 unused
  size_t start = 0, idx = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    size_t end = (nl == std::string_view::npos) ? text.size() : nl;
    std::string_view line = text.substr(start, end - start);
    long v = 0;
    if ((idx + 1) < safe.size() && safe[idx + 1] && line_has_marker(line)) {
      size_t d = line.size();
      while (d > 0 && std::isdigit(static_cast<unsigned char>(line[d - 1]))) d--;
      // A user comment can imitate a marker with an absurd number — treat an
      // unparseable value as "no marker" rather than aborting the compile.
      auto digits = line.substr(d);
      if (std::from_chars(digits.data(), digits.data() + digits.size(), v)
              .ec != std::errc{}) {
        v = 0;
      }
    }
    map.push_back(v);
    if (nl == std::string_view::npos) break;
    start = nl + 1;
    idx++;
  }
  return map;
}

// Original line for a node parsed from marker-annotated text: the marker on
// its line, else its own line (identity for the original, unannotated file).
inline long marker_orig_line(std::string_view text, size_t line) {
  auto map = marker_line_map(text);
  return (line < map.size() && map[line]) ? map[line]
                                          : static_cast<long>(line);
}

// Cached `#@culebra:N` marker lookup over one fragment's source, shared by the effects
// lowerer and the generator CpsBuilder. `src_is_original` makes an unmarked
// line fall back to its own (identity) line; otherwise it has no provenance.
struct LineMarkers {
  std::string_view src;
  bool src_is_original = false;
  std::vector<long> cache;
  bool built = false;
  long orig_line(const peg::Ast& n) {
    if (!built) {
      cache = marker_line_map(src);
      built = true;
    }
    long m = (n.line < cache.size()) ? cache[n.line] : 0;
    if (m) return m;
    return src_is_original ? static_cast<long>(n.line) : 0;
  }
  // Marker suffix to re-attach when emitting `n`'s source onto a fresh line (a
  // single-line statement's trailing marker sits outside its node span, so a
  // plain slice loses it).
  std::string mk(const peg::Ast& n) {
    long p = orig_line(n);
    return p ? line_marker(p) : "";
  }
};

// Rebuild `n`'s subtree with original line numbers: a node's line maps through
// `map`, or `fallback` (the construct's declaration line) when its line is
// synthesized. Subtrees spliced in from other fragments — identified by a
// different parse `path` label — are already repositioned and returned as-is.
// peg::Ast's line is const, so nodes are recreated via the two-step ctor pair
// (full ctor sets name; the copy ctor restores original_name / original_tag).
inline std::shared_ptr<peg::Ast> reposition_ast(
    const std::shared_ptr<peg::Ast>& n, const std::vector<long>& map,
    long fallback, const std::string& label) {
  if (n->path != label) return n;
  long line = (n->line < map.size() && map[n->line])
                  ? map[n->line]
                  : fallback;
  std::shared_ptr<peg::Ast> out;
  if (n->is_token) {
    peg::Ast tmp(n->path.c_str(), static_cast<size_t>(line), n->column,
                 n->name.c_str(), n->token, n->position, n->length,
                 n->choice_count, n->choice, n->preserve_position);
    out = std::make_shared<peg::Ast>(tmp, n->original_name.c_str(),
                                     n->position, n->length,
                                     n->original_choice_count,
                                     n->original_choice);
  } else {
    std::vector<std::shared_ptr<peg::Ast>> kids;
    kids.reserve(n->nodes.size());
    for (auto& c : n->nodes) {
      kids.push_back(reposition_ast(c, map, fallback, label));
    }
    peg::Ast tmp(n->path.c_str(), static_cast<size_t>(line), n->column,
                 n->name.c_str(), kids, n->position, n->length,
                 n->choice_count, n->choice, n->preserve_position);
    out = std::make_shared<peg::Ast>(tmp, n->original_name.c_str(),
                                     n->position, n->length,
                                     n->original_choice_count,
                                     n->original_choice);
  }
  for (auto& c : out->nodes) c->parent = out;
  return out;
}

// Unique per-fragment parse label, so `reposition_ast` can tell this
// fragment's nodes from subtrees spliced in by nested lowering.
inline std::string next_fragment_label(const char* stem) {
  static int counter = 0;
  return std::format("<{}#{}>", stem, counter++);
}

inline std::string rewrite_locals_to_this(std::string_view src,
                                          const std::set<std::string>& names) {
  // The two declaration-strip patterns are name-independent — hoist
  // them to file scope so each Stage 2 transform doesn't recompile
  // them. `std::regex` construction is the famously expensive part.
  static const std::regex strip_let_this(R"(\blet\s+this\.)");
  static const std::regex strip_mut_this(R"(\bmut\s+this\.)");
  std::vector<std::string> sorted(names.begin(), names.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.size() > b.size(); });
  // Build each name's pattern once per call and reuse it across code spans —
  // `std::regex` construction is the expensive part (see the two static strips
  // above); the patterns are name-dependent so they can't be file-scope static.
  // Group 1 captures the boundary (start-of-string, the `..` range operator, or
  // any byte that is neither `.` nor an identifier char) so it can be restored
  // ahead of the inserted `this.`. `..` is listed before the single-char class
  // so a range bound is matched as an operand rather than a member access on
  // its second dot.
  std::vector<std::regex> pats;
  pats.reserve(sorted.size());
  for (const auto& name : sorted) {
    pats.emplace_back("(^|\\.\\.|[^.A-Za-z0-9_])" + name + "\\b");
  }
  // Rewrite only the code spans (see rewrite_outside_strings): an identifier
  // that appears as literal text inside a string / comment is left untouched.
  auto rewrite_code = [&](std::string_view code) -> std::string {
    std::string out(code);
    for (size_t k = 0; k < sorted.size(); k++) {
      out = std::regex_replace(out, pats[k], "$1this." + sorted[k]);
    }
    out = std::regex_replace(out, strip_let_this, "this.");
    out = std::regex_replace(out, strip_mut_this, "this.");
    return out;
  };
  return rewrite_outside_strings(src, rewrite_code);
}

// Collect every name introduced by a `let`/`mut` decl in a fn body
// (single-lvalue form only; multi-lvalue chains skipped). Stops at
// nested fn boundaries.
inline std::set<std::string> collect_local_names(const peg::Ast& body) {
  using namespace peg::udl;
  std::set<std::string> out;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    // A nested `handle` opens its own computation scope; its locals belong to
    // that inner computation, not this one (they must not be rewritten to this
    // instance's `this.`). Generators carry no HANDLE, so this is a no-op there.
    if (is_fn_boundary(n.tag) || n.tag == "HANDLE"_) return;
    if (n.tag == "ASSIGNMENT"_) {
      auto av = view_assignment(n);
      if ((av.is_let || av.is_mut) && av.lvalcnt == 1) {
        const auto& ident = *n.nodes[av.lvaloff];
        if (ident.tag == "IDENTIFIER"_) out.insert(std::string(ident.token));
      }
    }
    for (auto& c : n.nodes) walk(*c);
  };
  walk(body);
  return out;
}

// Unwrap a STATEMENT wrapper down to the concrete tag (CLASS_DECL,
// WHILE, ASSIGNMENT, ...). AstOptimizer collapses most STATEMENTs
// already, but the wrapper survives for the choice-tagged variants.
inline const peg::Ast* unwrap_stmt(const peg::Ast* s) {
  using namespace peg::udl;
  if (s && s->tag == "STATEMENT"_ && !s->nodes.empty()) {
    return s->nodes[0].get();
  }
  return s;
}

// View the children of a fn body (BLOCK / STATEMENTS / single stmt) as
// a flat vector of statement-shaped nodes.
inline std::vector<const peg::Ast*> body_stmts(const peg::Ast& body) {
  using namespace peg::udl;
  std::vector<const peg::Ast*> out;
  if (body.tag == "STATEMENTS"_) {
    for (auto& c : body.nodes) out.push_back(c.get());
  } else {
    out.push_back(&body);
  }
  return out;
}

// --- Stage 5: for-in desugar to while + iterator -------------------------

// Outermost FOR nodes (within a single fn) that contain at least one YIELD
// somewhere in their subtree. Stops at fn boundaries and does NOT descend
// into yielding FORs — those are rewritten as a unit, so any inner FOR
// nested within them is left alone for a follow-up pass. The walker only
// surfaces FORs whose yields belong to *this* generator.
inline std::vector<const peg::Ast*> collect_outermost_yielding_fors(
    const peg::Ast& body) {
  using namespace peg::udl;
  std::vector<const peg::Ast*> out;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    if (is_fn_boundary(n.tag)) return;
    if (n.tag == "FOR"_ && fn_body_has_yield(n)) {
      out.push_back(&n);
      return;
    }
    for (auto& c : n.nodes) walk(*c);
  };
  walk(body);
  return out;
}

// Source-level rewrite: each outermost yielding `for x in expr BODY` becomes
// `let _g_it_<pos> = (expr).iter()
//  while _g_it_<pos>.has_next() {
//    let x = _g_it_<pos>.next()
//    BODY_inner
//  }`
// Returns the rewritten body source, or `nullopt` when there were no
// yielding FORs to rewrite (so callers can gate the desugar pipeline on a
// single walk). Iterator variable names use the FOR's source position to
// stay unique. The result has fresh source positions once re-parsed;
// callers must re-parse before walking the AST again.
inline std::optional<std::string> rewrite_yielding_fors_to_while(
    const peg::Ast& body, const char* src, size_t src_len) {
  auto fors = collect_outermost_yielding_fors(body);
  if (fors.empty()) return std::nullopt;
  std::string out(ast_source_slice(body, src, src_len));
  size_t base = body.position;
  auto marker_map = marker_line_map({src, src_len});  // loop-invariant
  for (auto it = fors.rbegin(); it != fors.rend(); ++it) {
    auto* f = *it;
    if (f->nodes.size() < 3) continue;
    const auto& var_node = *f->nodes[0];
    const auto& expr_node = *f->nodes[1];
    const auto& blk_node = *f->nodes[2];
    auto expr_sv = ast_source_slice(expr_node, src, src_len);
    auto blk_inner = strip_block_braces(
        ast_source_slice(blk_node, src, src_len));
    auto iter_var = std::format("_g_it_{}", f->position);
    // The three synthesized lines carry the `for`'s provenance marker so an
    // error in the loop machinery (e.g. `.iter()` on a non-iterable) reports
    // the for-in's original line.
    long orig = (f->line < marker_map.size() && marker_map[f->line])
                    ? marker_map[f->line]
                    : static_cast<long>(f->line);
    std::string body_text(blk_inner);
    // A single-line body's trailing marker sits outside the for's span —
    // re-attach it so the loop body keeps its provenance.
    if (body_text.find('\n') == std::string::npos)
      body_text += line_marker(orig);
    auto replacement = std::format(
        "let {0} = ({1}).iter(){4}\n"
        "while {0}.has_next() {{{4}\n"
        "  let {2} = {0}.next(){4}\n"
        "  {3}\n"
        "}}",
        iter_var, std::string(expr_sv),
        std::string(var_node.token), body_text, line_marker(orig));
    out.replace(f->position - base, f->length, replacement);
  }
  return out;
}

// --- Transformation entry points -----------------------------------------

// Re-parse a `fn __gen_wrapper__(...) { ... }` source fragment and return
// its MULTIFN_DECL node, after registering the source for lifetime. peg::Ast
// holds string_views into the source, so the synthesized buffer must stay
// alive until the AST is discarded — generator_transform_sources() owns it.
// Parse a synthesized buffer after registering it for process lifetime (the
// resulting AST's string_views point into it). The single place that pairs the
// lifetime store with `parse` — used by both the generator wrapper parse and
// the effects transform's body re-parse, so the "register before parse" rule
// lives in one spot.
// Ledger from parse label -> the fragment buffer it was parsed from. A
// transform that walks a tree with spliced-in subtrees (identified by a
// different `node->path`) resolves the right slice base here — e.g. the
// effects pass reaching a construct inside a generator-lowered body.
// Non-unique labels (internal re-parses that never splice nodes into the
// final AST) may overwrite each other; only `next_fragment_label` labels
// are ever looked up.
inline std::map<std::string, std::shared_ptr<std::string>, std::less<>>&
fragment_source_registry() {
  static std::map<std::string, std::shared_ptr<std::string>, std::less<>> reg;
  return reg;
}

inline std::shared_ptr<peg::Ast> parse_registered_source(
    const char* label, std::shared_ptr<std::string> synthesized) {
  generator_transform_sources().push_back(synthesized);
  fragment_source_registry()[label] = synthesized;
  std::vector<std::string> msgs;
  return parse(label, synthesized->data(), synthesized->size(), msgs);
}

inline std::shared_ptr<peg::Ast> parse_wrapper_fn(
    std::shared_ptr<std::string> synthesized,
    const char* label = "<generator-transform>") {
  using namespace peg::udl;
  auto sub_ast = parse_registered_source(label, synthesized);
  if (!sub_ast) return nullptr;
  std::shared_ptr<peg::Ast> wrapper_fn;
  std::function<void(std::shared_ptr<peg::Ast>)> walk =
      [&](std::shared_ptr<peg::Ast> n) {
        if (wrapper_fn) return;
        if (n->tag == "MULTIFN_DECL"_) { wrapper_fn = n; return; }
        for (auto& c : n->nodes) walk(c);
      };
  walk(sub_ast);
  return wrapper_fn;
}

// Replace `ast->nodes.back()` (the original body BLOCK) with the BLOCK
// from a freshly-parsed `fn __gen_wrapper__() { ... }` source fragment.
// Shared by Stage 1 and Stage 2 — both end with "now swap the body".
inline bool swap_body_from_wrapper(std::shared_ptr<peg::Ast> ast,
                                   std::shared_ptr<std::string> synthesized) {
  auto wrapper_fn = parse_wrapper_fn(synthesized);
  if (!wrapper_fn) return false;
  ast->nodes.back() = wrapper_fn->nodes.back();
  return true;
}

// Replace `ast`'s PARAMETERS (at `params_idx`) and body with those from a
// freshly-parsed `fn __gen_wrapper__(<params>) { <body> }` source. Keeps the
// original name (and decorators) intact, while giving downstream stages an
// AST whose param + body positions point into the synthesized source — the
// shape Stage 3 needs after the Stage 5 for-in desugar rewrites the body.
inline bool swap_body_with_wrapper_params(
    std::shared_ptr<peg::Ast> ast,
    std::shared_ptr<std::string> synthesized,
    size_t params_idx) {
  auto wrapper_fn = parse_wrapper_fn(synthesized);
  if (!wrapper_fn || wrapper_fn->nodes.size() < 3) return false;
  if (ast->nodes.size() <= params_idx + 1) return false;
  ast->nodes[params_idx + 1] = wrapper_fn->nodes[1];
  ast->nodes.back() = wrapper_fn->nodes.back();
  return true;
}

// Wire the generated class's `new(_p0, _p1, ...)` slot list and append
// matching `this.<param> = _pN` initializers, then `this.<local> = nil`
// for every body-declared local not shadowed by a param. Shared between
// Stage 3 and Stage 6b — both stages seed `ctor_inits` with their own
// state-field prefix (`_g_drained` / `_g_phase` / etc.) before calling
// this to append the per-instance bindings.
// Positional parameter names of a PARAMETERS node, skipping the kw-only
// separator and any `**kwargs` rest. Shared by the generator and effects
// transforms — both feed the names into ctor slot emission, so the skip rules
// must stay in lockstep.
inline std::vector<std::string_view> collect_positional_param_names(
    const peg::Ast& params_ast) {
  std::vector<std::string_view> names;
  for (const auto& pn : params_ast.nodes) {
    if (is_kw_only_sep(*pn) || is_kwargs_rest(*pn)) continue;
    names.push_back(view_parameter(*pn).name);
  }
  return names;
}

inline void emit_ctor_param_and_local_inits(
    const std::vector<std::string_view>& param_names,
    const std::set<std::string>& locals,
    std::string& ctor_params,
    std::string& ctor_call_args,
    std::string& ctor_inits) {
  std::set<std::string> param_set;
  for (size_t j = 0; j < param_names.size(); j++) {
    auto pn = std::string(param_names[j]);
    auto slot = std::format("_p{}", j);
    if (j > 0) {
      ctor_params += ", ";
      ctor_call_args += ", ";
    }
    ctor_params += slot;
    ctor_call_args += pn;
    ctor_inits += std::format("      this.{} = {}\n", pn, slot);
    param_set.insert(std::move(pn));
  }
  for (const auto& l : locals) {
    if (!param_set.contains(l)) {
      ctor_inits += std::format("      this.{} = nil\n", l);
    }
  }
}

// --- CPS engine: flat-dispatch state machine -----------------------------
//
// Linearizes a generator body into a flat list of states. Each state is a
// culebra statement sequence ending in a transition: a yield (set
// lookahead, advance state, `return true`), a delegate hand-off
// (`yield from`), or a `this._g_state = K; continue` jump. has_next() is
// one `while true` dispatch loop over the states. Because culebra's yield
// is a STATEMENT (never an expression) and locals live on the heap
// (this.X), the linearizer works statement-by-statement with no
// expression splitting and no liveness analysis; "flat" dispatch (every
// basic block is a state, edges set a counter) sidesteps the relooper
// problem of reconstructing structured loops. See
// [[project-generator-design]] §CPS.
//
// Handles plain stmts / yield / yield from / if-elseif-else / while
// (incl. nested) / break / continue / return / defer. for-in is
// desugared to while by an upstream pre-pass. A yielding `match` arm is
// grammatically impossible (yield is a statement, match arms are
// expressions), so match never carries a yield to lower.

// True if `node` contains a break/continue that targets an *enclosing*
// loop (i.e. not nested inside a while/for within `node`), or a return
// anywhere. Such statements can't be emitted verbatim into the dispatch
// loop — break/continue/return must become state jumps. A self-contained
// inner loop with its own internal break stays verbatim-safe (the break
// binds to that real inner loop). Stops at fn boundaries.
inline bool has_escaping_loop_ctrl(const peg::Ast& node, int loop_depth = 0) {
  using namespace peg::udl;
  if (is_fn_boundary(node.tag)) return false;
  if (node.tag == "RETURN"_) return true;
  if ((node.tag == "BREAK"_ || node.tag == "CONTINUE"_) && loop_depth == 0) {
    return true;
  }
  int inner = (node.tag == "WHILE"_ || node.tag == "FOR"_) ? loop_depth + 1
                                                           : loop_depth;
  for (auto& c : node.nodes) {
    if (has_escaping_loop_ctrl(*c, inner)) return true;
  }
  return false;
}

struct CpsBuilder {
  const char* src;
  size_t src_len;
  const std::set<std::string>& rewrite_set;
  std::vector<std::string> states;
  int terminal = -1;  // state that sets drained + returns false
  // (header, exit) of each enclosing CPS-managed loop, innermost last.
  std::vector<std::pair<int, int>> loop_stack;
  // `defer { B }` bodies, in source order. Each is registered (a
  // `_g_defer_K` flag set true) when its state is reached and run at
  // dispose in reverse (LIFO).
  std::vector<std::string> defer_bodies;
  bool failed = false;

  int fresh() {
    states.emplace_back();
    return static_cast<int>(states.size()) - 1;
  }
  std::string rw(const peg::Ast& n) {
    return rewrite_locals_to_this(ast_source_slice(n, src, src_len),
                                  rewrite_set);
  }
  LineMarkers markers{{src, src_len}};
  std::string mk(const peg::Ast& n) { return markers.mk(n); }
  // A fresh state that just jumps to `target` (break/continue/return).
  int jump_state(int target) {
    int e = fresh();
    states[e] = std::format("      this._g_state = {}\n      continue\n",
                            target);
    return e;
  }

  // True if this statement needs structural compilation: it contains a
  // yield anywhere, a break/continue/return escaping to an enclosing loop
  // / the generator, or a defer (which must register into the dispose
  // registry, not fire as a has_next-scope defer). Everything else is
  // verbatim-safe.
  static bool needs_split(const peg::Ast& s) {
    return fn_body_has_yield(s) || has_escaping_loop_ctrl(s) ||
           !collect_defers(s).empty();
  }

  // Linearize `stmts`, returning the entry state. `cont` is the state to
  // jump to after the sequence completes. Maximal runs of yield-free
  // statements collapse into a single state.
  int compile_seq(const std::vector<const peg::Ast*>& stmts, int cont) {
    int k = cont;
    std::string pending;
    auto flush = [&]() {
      if (pending.empty()) return;
      int s = fresh();
      states[s] = pending +
                  std::format("      this._g_state = {}\n      continue\n", k);
      k = s;
      pending.clear();
    };
    for (size_t idx = stmts.size(); idx-- > 0;) {
      const peg::Ast* s = stmts[idx];
      if (!needs_split(*s)) {
        pending = "      " + rw(*s) + mk(*s) + "\n" + pending;
      } else {
        flush();
        k = compile_stmt(s, k);
        if (failed) return -1;
      }
    }
    flush();
    return k;
  }

  int compile_stmt(const peg::Ast* s, int cont) {
    using namespace peg::udl;
    auto* u = unwrap_stmt(s);
    if (u->tag == "YIELD"_ && !u->nodes.empty()) {
      int e = fresh();
      states[e] = std::format(
          "      this._g_la = ({}){}\n"
          "      this._g_has_la = true\n"
          "      this._g_state = {}\n"
          "      return true\n",
          rw(*u->nodes[0]), mk(*u->nodes[0]), cont);
      return e;
    }
    if (u->tag == "YIELD_FROM"_ && !u->nodes.empty()) {
      int e = fresh();
      states[e] = std::format(
          "      this._g_delegate = ({}).iter(){}\n"
          "      this._g_state = {}\n"
          "      continue\n",
          rw(*u->nodes[0]), mk(*u->nodes[0]), cont);
      return e;
    }
    if (u->tag == "BREAK"_) {
      if (loop_stack.empty()) { failed = true; return -1; }
      return jump_state(loop_stack.back().second);
    }
    if (u->tag == "CONTINUE"_) {
      if (loop_stack.empty()) { failed = true; return -1; }
      return jump_state(loop_stack.back().first);
    }
    if (u->tag == "RETURN"_) {
      // Generators ignore a return value; `return` just ends iteration.
      return jump_state(terminal);
    }
    if (u->tag == "DEFER"_ && !u->nodes.empty()) {
      // Register the defer body when reached; dispose runs it (LIFO).
      int k = static_cast<int>(defer_bodies.size());
      defer_bodies.push_back(rewrite_locals_to_this(
          strip_block_braces(ast_source_slice(*u->nodes[0], src, src_len)),
          rewrite_set));
      int e = fresh();
      states[e] = std::format(
          "      this._g_defer_{} = true\n      this._g_state = {}\n"
          "      continue\n",
          k, cont);
      return e;
    }
    if (u->tag == "IF"_) return compile_if(u, cont);
    if (u->tag == "WHILE"_ && u->nodes.size() >= 2) {
      int h = fresh();
      loop_stack.push_back({h, cont});
      int body_entry = compile_seq(body_stmts(*u->nodes[1]), h);
      loop_stack.pop_back();
      if (failed) return -1;
      states[h] = std::format(
          "      if {} {{ this._g_state = {} }} else {{ this._g_state = {} }}{}\n"
          "      continue\n",
          rw(*u->nodes[0]), body_entry, cont, mk(*u->nodes[0]));
      return h;
    }
    if (u->tag == "LEXICAL_SCOPE"_ || u->tag == "STATEMENTS"_) {
      return compile_seq(body_stmts(*u), cont);
    }
    failed = true;  // anything unexpected
    return -1;
  }

  // if / else-if / else chain. IF nodes are [cond, block, cond, block, ...,
  // elseblock?]; a trailing odd node is the bare `else` block.
  int compile_if(const peg::Ast* ifnode, int cont) {
    const auto& nodes = ifnode->nodes;
    size_t n = nodes.size();
    int else_entry;
    size_t pairs;
    if (n % 2 == 1) {
      else_entry = compile_seq(body_stmts(*nodes[n - 1]), cont);
      if (failed) return -1;
      pairs = (n - 1) / 2;
    } else {
      else_entry = cont;
      pairs = n / 2;
    }
    int chain = else_entry;
    for (size_t p = pairs; p-- > 0;) {
      int block_entry = compile_seq(body_stmts(*nodes[2 * p + 1]), cont);
      if (failed) return -1;
      int s = fresh();
      states[s] = std::format(
          "      if {} {{ this._g_state = {} }} else {{ this._g_state = {} }}{}\n"
          "      continue\n",
          rw(*nodes[2 * p]), block_entry, chain, mk(*nodes[2 * p]));
      chain = s;
    }
    return chain;
  }
};

// CPS transform entry. Returns the transformed ast on success, or the
// original ast unchanged when the body uses a construct outside the
// engine's scope (caller then reports / falls back).
inline std::shared_ptr<peg::Ast> transform_one_generator_fn_cps(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len,
    const peg::Ast& name_ast, const peg::Ast& params_ast,
    long decl_fallback) {
  using namespace peg::udl;

  auto gen_name = std::format("_Gen_{}_{}_{}",
                              std::string(name_ast.token),
                              name_ast.line, name_ast.column);

  auto param_names = collect_positional_param_names(params_ast);
  auto locals = collect_local_names(*ast->nodes.back());
  std::set<std::string> rewrite_set = locals;
  for (auto& pn : param_names) rewrite_set.insert(std::string(pn));

  CpsBuilder b{src, src_len, rewrite_set, {}};
  b.terminal = b.fresh();
  b.states[b.terminal] =
      "      this._g_drained = true\n      return false\n";
  int entry = b.compile_seq(body_stmts(*ast->nodes.back()), b.terminal);
  if (b.failed || entry < 0) return ast;  // unsupported shape

  std::string ctor_params;
  std::string ctor_call_args;
  std::string ctor_inits = std::format(
      "      this._g_drained = false\n"
      "      this._g_has_la = false\n"
      "      this._g_la = nil\n"
      "      this._g_disposed = false\n"
      "      this._g_delegate = nil\n"
      "      this._g_state = {}\n",
      entry);
  for (size_t k = 0; k < b.defer_bodies.size(); k++) {
    ctor_inits += std::format("      this._g_defer_{} = false\n", k);
  }
  emit_ctor_param_and_local_inits(param_names, locals,
                                  ctor_params, ctor_call_args, ctor_inits);

  std::string dispatch;
  for (size_t id = 0; id < b.states.size(); id++) {
    dispatch += std::format(
        "      if this._g_state == {} {{\n{}      }}\n", id, b.states[id]);
  }

  // Registered defers run LIFO at dispose, each gated on its reach flag.
  // `defer_bodies` is already in reverse source order (compile_seq walks
  // statements back-to-front), so iterating it forward IS the LIFO order.
  std::string defer_runs;
  for (size_t k = 0; k < b.defer_bodies.size(); k++) {
    defer_runs += std::format(
        "      if this._g_defer_{} {{ {} }}\n", k, b.defer_bodies[k]);
  }

  auto synthesized = std::make_shared<std::string>(std::format(
      "fn __gen_wrapper__() {{\n"
      "  class {0} {{\n"
      "    new({1}) {{\n{2}    }}\n"
      "    iter() {{ this }}\n"
      "    has_next() {{\n"
      "      while true {{\n"
      "        if this._g_drained {{ return false }}\n"
      "        if this._g_has_la {{ return true }}\n"
      "        if this._g_delegate != nil {{\n"
      "          if this._g_delegate.has_next() {{\n"
      "            this._g_la = this._g_delegate.next()\n"
      "            this._g_has_la = true\n"
      "            return true\n"
      "          }}\n"
      "          if this._g_delegate.has('dispose') {{ this._g_delegate.dispose() }}\n"
      "          this._g_delegate = nil\n"
      "        }}\n"
      "{3}"
      "      }}\n"
      "    }}\n"
      "    next() {{\n"
      "      if !this._g_has_la {{ this.has_next() }}\n"
      "      let _v = this._g_la\n"
      "      this._g_la = nil\n"
      "      this._g_has_la = false\n"
      "      return _v\n"
      "    }}\n"
      "    dispose() {{\n"
      "      if this._g_disposed {{ return nil }}\n"
      "      this._g_disposed = true\n"
      "      this._g_drained = true\n"
      "      if this._g_delegate != nil {{\n"
      "        if this._g_delegate.has('dispose') {{ this._g_delegate.dispose() }}\n"
      "        this._g_delegate = nil\n"
      "      }}\n"
      "{5}"
      "    }}\n"
      "  }}\n"
      "  {0}.new({4})\n"
      "}}\n",
      gen_name, ctor_params, ctor_inits, dispatch, ctor_call_args,
      defer_runs));
  // Final parse: read the provenance markers back and rebuild the body with
  // original line numbers (machinery lines fall back to the fn's decl line).
  auto label = next_fragment_label("gen");
  auto wrapper_fn = parse_wrapper_fn(synthesized, label.c_str());
  if (!wrapper_fn) return ast;
  auto map = marker_line_map(*synthesized);
  ast->nodes.back() = reposition_ast(wrapper_fn->nodes.back(), map,
                                     decl_fallback, label);
  return ast;
}

// Dispatcher: lower a yield-carrying fn to the flat-dispatch CPS state
// machine. Two source-level pre-passes run first: the C# rule rejects
// yield inside try-catch/defer, and the for-in desugar rewrites every
// yielding `for x in e` to `while it.has_next()` form (to a fixpoint so
// nested for-ins are covered) since the CPS engine works over while.
// Everything else — straight-line yields, while/if branching, multiple
// yields per iteration, post-loop tails, break/continue/return, defer,
// yield from — is handled by the one engine.
inline std::shared_ptr<peg::Ast> transform_one_generator_fn(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len) {
  using namespace peg::udl;
  size_t i = 0;
  while (i < ast->nodes.size() && ast->nodes[i]->tag == "DECORATOR"_) i++;
  if (i + 2 >= ast->nodes.size()) return ast;
  if (!fn_body_has_yield(*ast->nodes.back())) return ast;

  // C# rule (CS1626): a yield statement may not appear inside a try-catch
  // or defer block. yield-spanning try is permanently out of scope
  // ([[project-generator-design]] §仕様凍結項目). Cleanup belongs in a
  // top-level `defer { ... }`; value-level recovery in the yielded
  // expression (`yield try { ... } catch e { ... }`).
  if (auto* bad = find_yield_inside_try_or_defer(*ast->nodes.back())) {
    throw CulebraError(
        "SyntaxError",
        "yield cannot appear inside a try-catch or defer block. Move "
        "the try to the yielded expression value (yield try { ... } "
        "catch e { ... }) or use a top-level `defer { ... }` for cleanup.",
        static_cast<long>(bad->line),
        static_cast<long>(bad->column));
  }

  // A self-contained `handle { … }` expression inside a generator body is
  // fine: the effects pass (which runs after this one) follows the fragment
  // registry to slice it from the right buffer. An `effect fn` DECL is not —
  // it would lower to a named fn in a state block (the same state-scope
  // problem that rejects plain named fn decls below); a bare `perform`
  // outside any handle is rejected by the effects pass itself.
  {
    using namespace peg::udl;
    std::function<const peg::Ast*(const peg::Ast&)> find_eff_decl =
        [&](const peg::Ast& n) -> const peg::Ast* {
      if (n.tag == "EFFECT_FN_DECL"_) return &n;
      for (auto& c : n.nodes) {
        if (auto* e = find_eff_decl(*c)) return e;
      }
      return nullptr;
    };
    if (auto* e = find_eff_decl(*ast->nodes.back())) {
      throw CulebraError(
          "SyntaxError",
          "an `effect fn` declaration cannot appear inside a generator body — "
          "define it outside the generator.",
          static_cast<long>(e->line), static_cast<long>(e->column));
    }
  }

  // A named function definition inside a generator body has no good CPS
  // lowering — the JIT doesn't bind it in the generator's state frame, so it
  // raised NameError while the interp ran it, an interp/JIT divergence. Reject
  // it uniformly (like the yield-in-try rule). Anonymous fn / lambda VALUES
  // (`let f = |x| ...` / `let f = fn (x) { ... }`) work and are unaffected.
  if (auto* fd = find_nested_fndef(*ast->nodes.back())) {
    throw CulebraError(
        "SyntaxError",
        "a named function definition cannot appear inside a generator body "
        "(a function that uses yield). Bind a lambda instead (let f = |x| ... "
        "/ let f = fn (x) { ... }) or define the function outside the "
        "generator.",
        static_cast<long>(fd->line),
        static_cast<long>(fd->column));
  }

  // Annotate every body line with a `#@culebra:<original-line>` provenance marker
  // before any rewriting stage. Markers are comments, so each later stage
  // (for-desugar, CPS state emission) carries them for free, and the final
  // fragment parse can restore original line numbers (see reposition below).
  // A body sliced out of an already-annotated fragment (a generator fn nested
  // in an effect body) keeps its markers as-is: re-annotating would stamp
  // fragment-relative numbers onto the marker-less machinery lines.
  long decl_fallback = static_cast<long>(ast->nodes[i]->line);
  {
    auto inner = strip_block_braces(
        ast_source_slice(*ast->nodes.back(), src, src_len));
    if (!text_has_line_marker(inner)) {
      long first = line_of_offset({src, src_len},
                                  static_cast<size_t>(inner.data() - src));
      auto params_sv = ast_source_slice(*ast->nodes[i + 1], src, src_len);
      auto annotated = std::make_shared<std::string>(std::format(
          "fn __gen_wrapper__{} {{\n{}\n}}\n", std::string(params_sv),
          annotate_line_markers(inner, first)));
      if (!swap_body_with_wrapper_params(ast, annotated, i)) return ast;
      src = annotated->data();
      src_len = annotated->size();
    } else {
      // Already-annotated fragment: map the decl line through its markers
      // while `src` is still the buffer the name node indexes.
      decl_fallback = marker_orig_line({src, src_len}, ast->nodes[i]->line);
    }
  }

  // Desugar yielding for-in loops to while + iterator, re-parsing each
  // time, until none remain (nested for-ins surface as outermost on the
  // next pass). After the swap, params/body positions point into the
  // synthesized source, so `src`/`src_len` track it.
  while (auto rewritten = rewrite_yielding_fors_to_while(
             *ast->nodes.back(), src, src_len)) {
    auto params_sv = ast_source_slice(*ast->nodes[i + 1], src, src_len);
    auto body_inner = strip_block_braces(*rewritten);
    auto desugared = std::make_shared<std::string>(std::format(
        "fn __gen_wrapper__{} {{\n{}\n}}\n",
        std::string(params_sv), std::string(body_inner)));
    if (!swap_body_with_wrapper_params(ast, desugared, i)) return ast;
    src = desugared->data();
    src_len = desugared->size();
    if (!fn_body_has_yield(*ast->nodes.back())) return ast;
  }

  const auto& name_ast = *ast->nodes[i];
  const auto& params_ast = *ast->nodes[i + 1];
  auto orig_body = ast->nodes.back();
  auto out = transform_one_generator_fn_cps(ast, src, src_len, name_ast,
                                            params_ast, decl_fallback);
  if (out->nodes.back().get() != orig_body.get()) return out;

  // CPS left the body untouched — it hit a construct it can't lower
  // (e.g. a yielding `match` arm, which the grammar already forbids, or a
  // break/continue outside any loop).
  throw CulebraError(
      "SyntaxError",
      "unsupported control flow in a generator body (yield reachable "
      "through a construct the generator transform can't lower).",
      static_cast<long>(name_ast.line),
      static_cast<long>(name_ast.column));
}

// Walk the AST, transforming every yield-carrying MULTIFN_DECL. The
// walk visits every node — yield-free modules pay one whole-tree
// pointer pass (dwarfed by the PEG parse already run).
inline std::shared_ptr<peg::Ast> transform_generators_in(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len) {
  using namespace peg::udl;
  for (auto& child : ast->nodes) {
    child = transform_generators_in(child, src, src_len);
  }
  if (ast->tag == "MULTIFN_DECL"_) {
    auto& body = ast->nodes.back();
    if (fn_body_has_yield(*body)) {
      return transform_one_generator_fn(ast, src, src_len);
    }
  }
  return ast;
}

// Parse + the generator transformation pass. The public entry
// `parse_with_transforms` (effects_transform.h) chains the effects pass
// after this one; callers route through the public entry.
inline std::shared_ptr<peg::Ast> parse_with_generator_transforms(
    const std::string& path, const char* expr, size_t len,
    std::vector<std::string>& msgs) {
  auto ast = parse(path, expr, len, msgs);
  if (!ast) return ast;
  return transform_generators_in(ast, expr, len);
}

}  // namespace culebra
