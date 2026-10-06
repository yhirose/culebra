// What lowering a body to a state class is made of.
//
// The effects pass (effects_transform.h) rewrites an `effect fn` or a
// `handle` body, in source, into a class whose methods are the body's states,
// and re-parses the result. This header holds the parts of that which are not
// about effects: the line markers and anchors that lead a re-parsed
// fragment's positions back to the source it was written in, the locals a
// suspension splits moved onto the instance (PromotedLocals, read off
// resolve.h), the names visible around a body (Surroundings), and the loop
// bookkeeping of a state machine.
//
// Generators were lowered with the same parts until they kept their frames
// (generator_rules.h, docs/internals/vm.md §5.7), which is why they stand
// apart from the pass that uses them.

#pragma once

#include "frontend/fragments.h"
#include "frontend/generator_rules.h"
#include "frontend/lint.h"
#include "frontend/parser.h"
#include "frontend/resolve.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace culebra {

// Slice the source range an AST node spans. peg::Ast carries
// `position` + `length` in bytes; safe so long as `src` is the same
// buffer that produced `node`.
inline std::string_view ast_source_slice(const peg::Ast& node,
                                          const std::string& src) {
  if (node.position + node.length > src.size()) return {};
  return std::string_view(src.data() + node.position, node.length);
}

// --- Source-text helpers -------------------------------------------------

// The code of `s`, as [begin, end) spans offset by `base`: everything but the
// literal content of string constants and comments (line `#`/`//`, block
// `/* … */`), so an identifier that happens to appear as literal text is never
// taken for code. Interpolation `{expr}` inside a `"…"` / `"""…"""` string IS
// code (recursively, as an expr may embed further strings). Only the
// double-quoted forms interpolate; `'…'` / `` `…` `` are fully raw.
inline std::vector<std::pair<size_t, size_t>> code_spans(std::string_view s,
                                                         size_t base = 0) {
  std::vector<std::pair<size_t, size_t>> out;
  size_t i = 0, n = s.size(), code0 = 0;
  auto flush = [&](size_t end) {
    if (end > code0) out.push_back({base + code0, base + end});
  };
  while (i < n) {
    char c = s[i];
    bool line_comment = c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/');
    if (line_comment) {
      flush(i);
      while (i < n && s[i] != '\n') i++;
      code0 = i;
    } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {  // block comment
      flush(i);
      size_t j = i + 2;
      while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
      i = code0 = j + 1 < n ? j + 2 : n;
    } else if (c == '\'' || c == '`') {  // raw string, no interpolation
      flush(i);
      i = code0 = skip_string_literal(s, i);
    } else if (c == '"') {  // interpolated (or triple) string
      flush(i);
      bool triple = is_triple_quote(s, i);
      i += triple ? 3 : 1;
      while (i < n) {
        if (!triple && s[i] == '"') { i++; break; }
        if (triple && is_triple_quote(s, i)) { i += 3; break; }
        if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
        if (s[i] == '{') {  // interpolation expr — code, may embed strings
          size_t k = i + 1, depth = 1;
          while (k < n && depth > 0) {
            char d = s[k];
            if (d == '{') { depth++; k++; }
            else if (d == '}') { if (--depth == 0) break; k++; }
            else if (d == '"' || d == '\'' || d == '`') k = skip_string_literal(s, k);
            else k++;
          }
          auto hole = code_spans(s.substr(i + 1, k - (i + 1)), base + i + 1);
          out.insert(out.end(), hole.begin(), hole.end());
          i = k < n ? k + 1 : k;
        } else {
          i++;
        }
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
// The effects transform rewrites body SOURCE TEXT and re-parses it,
// so the re-parsed AST's line numbers are fragment-relative and every error
// reported through them used to point nowhere near the user's code. The fix is
// a `#line`-style provenance marker: each user code line gets a trailing
// ` #@culebra:<original-line>` comment when the body is first sliced out of
// the real file. Being a comment, the marker survives every later text stage
// (the effects for-in desugar, ANF hoisting, CPS state emission, nested
// re-lowering) for free —
// verbatim slices carry it, synthesized lines simply lack one. After the FINAL
// fragment parse, `marker_line_map` reads the markers back and
// `reposition_ast` rebuilds the AST with original line numbers. A node copied
// from user code gets its exact line and column from the anchors instead
// (fragments.h); the markers serve the machinery nodes around it, and those
// on a synthesized line fall back to the construct's declaration line.

// The marker token. Deliberately verbose so a user comment ending in a bare
// `#@123` can't be mistaken for provenance.
inline constexpr std::string_view kLineMarker = "#@culebra:";

// The ` #@culebra:N` suffix for a line whose original line is `n` — the one
// writer of the format `line_has_marker` / `marker_line_map` read back.
inline std::string line_marker(int64_t n) {
  return std::format(" {}{}", kLineMarker, n);
}

// 1-based line number of byte offset `pos` in `s`.
inline int64_t line_of_offset(std::string_view s, size_t pos) {
  int64_t line = 1;
  for (size_t i = 0; i < pos && i < s.size(); i++) {
    if (s[i] == '\n') line++;
  }
  return line;
}

// For each line of `s` (1-based; index 0 unused), whether its END (the `\n`,
// or EOF for the last line) sits in code / comment context — i.e. a trailing
// `#@culebra:N` marker comment may be placed or read there. A line ending inside a
// string literal (multi-line triple string) or an interpolation hole is
// unsafe. This walk mirrors `code_spans`' lane logic at line granularity; the
// two are kept as documented twins.
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

// The edits that append ` #@culebra:<line>` to each marker-safe line of
// bytes [begin, end) of `src` — the entry-point annotation for a body sliced
// out of the ORIGINAL file, so `line` is the line in `src`. Lines that already
// carry a marker (a body re-sliced from an annotated fragment never reaches
// here, but a user comment could imitate one) keep the existing value.
inline std::vector<SourceEdit> line_marker_edits(const std::string& src,
                                                 size_t begin, size_t end) {
  auto text = std::string_view(src).substr(begin, end - begin);
  auto first_line = line_of_offset(src, begin);
  auto safe = safe_line_ends(text);
  std::vector<SourceEdit> out;
  size_t start = 0, idx = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    size_t stop = (nl == std::string_view::npos) ? text.size() : nl;
    std::string_view line = text.substr(start, stop - start);
    bool ok = (idx + 1) < safe.size() && safe[idx + 1];
    if (ok && !line.empty() && !line_has_marker(line)) {
      out.push_back({begin + stop, 0,
                     line_marker(first_line + static_cast<long>(idx))});
    }
    if (nl == std::string_view::npos) break;
    start = nl + 1;
    idx++;
  }
  return out;
}

// Read the markers back: for each 1-based line of `text`, its original line,
// or 0 when the line carries none (synthesized machinery).
inline std::vector<int64_t> marker_line_map(std::string_view text) {
  auto safe = safe_line_ends(text);
  std::vector<int64_t> map;
  map.push_back(0);  // index 0 unused
  size_t start = 0, idx = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    size_t end = (nl == std::string_view::npos) ? text.size() : nl;
    std::string_view line = text.substr(start, end - start);
    int64_t v = 0;
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

// Cached `#@culebra:N` marker lookup over one fragment's source.
// `src_is_original` makes an unmarked line fall back to its own (identity)
// line; otherwise it has no provenance.
struct LineMarkers {
  std::string_view src;
  bool src_is_original = false;
  std::vector<int64_t> cache;
  bool built = false;
  int64_t orig_line(const peg::Ast& n) {
    if (!built) {
      cache = marker_line_map(src);
      built = true;
    }
    int64_t m = (n.line < cache.size()) ? cache[n.line] : 0;
    if (m) return m;
    return src_is_original ? static_cast<long>(n.line) : 0;
  }
  // Marker suffix to re-attach when emitting `n`'s source onto a fresh line (a
  // single-line statement's trailing marker sits outside its node span, so a
  // plain slice loses it).
  std::string mk(const peg::Ast& n) {
    int64_t p = orig_line(n);
    return p ? line_marker(p) : "";
  }
};

// Rebuild `n`'s subtree with original positions. A node copied from user code
// takes the line and column its anchor leads back to; any other maps its line
// through `map`, or `fallback` (the construct's declaration line) when its
// line is synthesized. Subtrees spliced in from other fragments — identified by
// a different parse `path` label — are already repositioned and returned
// as-is. peg::Ast's line is const, so nodes are recreated via the two-step
// ctor pair (full ctor sets name; the copy ctor restores original_name /
// original_tag).
inline std::shared_ptr<peg::Ast> reposition_ast(
    const std::shared_ptr<peg::Ast>& n, const std::string& text,
    const std::vector<int64_t>& map, int64_t fallback,
    const std::string& label, SourceResolver& resolver) {
  if (n->path != label) return n;
  size_t line = static_cast<size_t>(
      (n->line < map.size() && map[n->line]) ? map[n->line] : fallback);
  size_t column = n->column;
  if (auto at = resolver.resolve(text, *n)) {
    line = static_cast<size_t>(at->line);
    column = static_cast<size_t>(at->col);
  }
  std::shared_ptr<peg::Ast> out;
  if (n->is_token) {
    peg::Ast tmp(n->path.c_str(), line, column,
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
      kids.push_back(reposition_ast(c, text, map, fallback, label, resolver));
    }
    peg::Ast tmp(n->path.c_str(), line, column,
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

// Reposition a fragment's final parse, `n` being parsed from `text` under
// `label`.
inline std::shared_ptr<peg::Ast> reposition_fragment(
    const std::shared_ptr<peg::Ast>& n, const std::string& text,
    int64_t fallback, const std::string& label) {
  SourceResolver resolver;
  return reposition_ast(n, text, marker_line_map(text), fallback, label,
                        resolver);
}

// Unique per-fragment parse label, so `reposition_ast` can tell this
// fragment's nodes from subtrees spliced in by nested lowering.
inline std::string next_fragment_label(const char* stem) {
  // Atomic: concurrent isolates lower bodies at the same time, and two
  // fragments sharing a label would resolve to each other's buffer.
  static std::atomic<int> counter{0};
  return std::format("<{}#{}>", stem,
                     counter.fetch_add(1, std::memory_order_relaxed));
}

// A box holding `value` — the one shape every box is made in.
inline std::string box_literal(std::string_view value) {
  return std::format("{{mut v: {}}}", value);
}

// The locals a lowered body moves onto its state instance, by variable. The
// body is resolved (resolve.h) under the names visible around it, and a
// variable gets a slot when the machine enters the scope that holds it
// (promote): a scope whose statements end up in different states. A scope no
// suspension splits is emitted as it was written, and its variables stay the
// locals they were.
//
// A slotted variable some closure written in the body reads lives in a
// one-field box: the instance holds the box and the closure captures the box,
// so neither reaches the other. Unboxed, the closure would read the local
// through `self` while the instance held the closure back (`let h = fn …` is
// a local too) — a reference cycle for a body that never wrote one. A closure
// is any function scope of the resolution, a `defer` among them though these
// transforms inline one they run themselves: an extra box costs one object, a
// missing one restores the cycle.
class PromotedLocals {
 public:
  struct Slot {
    std::string field;  // on the instance; empty while the variable has none
    std::string box;    // the plain local its box is reached through, if boxed
    // Where the variable lives, as source. Each piece of emitted code binds
    // the box locals it names on entry (box_prologue), so one spelling serves
    // both inside a closure (which captures the local) and outside it.
    std::string place() const {
      return box.empty() ? "self." + field : box + ".v";
    }
  };

  PromotedLocals() = default;  // of no body: nothing has a slot
  // `body` under parameters named `params` (`mut_params` those declared
  // `mut`), where `around` are the names visible around the function. The
  // body's own level is entered.
  PromotedLocals(const peg::Ast& body, std::span<const std::string_view> params,
                 std::span<const std::string> around,
                 std::span<const std::string_view> mut_params = {})
      : res_(resolve::resolve_body(body, params, lint::resolve_options(around))),
        body_(res_.body_scope.at(&body)),
        slots_(res_.symbols.size()),
        captured_(res_.symbols.size()),
        declared_(res_.symbols.size()),
        mut_param_(res_.symbols.size()) {
    for (const auto& [node, use] : res_.uses)
      if (use.symbol != resolve::kNone && res_.frame_of(use.symbol) == body_ &&
          res_.function_of(use.scope) != body_ && !own_name(use))
        captured_[use.symbol] = true;
    using resolve::Form;
    for (const auto& d : res_.declarations) {
      declaring_.insert(d.node);
      declared_[d.symbol].push_back(
          {d.node->token.data(), d.form == Form::Mut ||
                                     d.form == Form::Pattern ||
                                     d.form == Form::Catch});
    }
    for (auto& ds : declared_)
      std::sort(ds.begin(), ds.end(),
                [](const auto& a, const auto& b) { return a.at < b.at; });
    for (auto name : mut_params)
      if (auto it = res_.scopes[body_].names.find(name);
          it != res_.scopes[body_].names.end())
        mut_param_[it->second] = true;
    promote(body_);
  }

  // The scope the body's own statements are on, and the one `block` opens
  // when it is a scope of its own (resolve::Resolution::block_scope).
  size_t body_scope() const { return body_; }
  std::optional<size_t> scope_of(const peg::Ast& block) const {
    auto it = res_.block_scope.find(&block);
    if (it == res_.block_scope.end()) return std::nullopt;
    return it->second;
  }

  // Give every variable of `scope` a slot. A user's local gets a namespace
  // of its own, apart from the state class's methods (`_step`), its
  // machinery fields and the names the runtime reads off an
  // object (`drop`); the second variable of a name gets a numbered one. The
  // machinery's own names (`_g_*`, `_eff_*`, like the `_eff_outer` a nested
  // handle is handed) are read back by their spelling and keep it.
  void promote(size_t scope) {
    for (const auto& [name, sym] : res_.scopes[scope].names) {
      Slot& s = slots_[sym];
      if (!s.field.empty()) continue;
      int k = spelled_[name]++;
      if (name.starts_with("_g_") || name.starts_with("_eff_")) {
        if (k)
          throw CulebraError("InternalError",
                             "a lowering declared `" + name + "` twice", 0, 0);
        s.field = name;
      } else {
        s.field = k ? std::format("_l{}_{}", k, name) : "_l_" + name;
      }
      if (captured_[sym])
        s.box = k ? std::format("_bx{}_{}", k, name) : "_bx_" + name;
      order_.push_back(sym);
    }
  }

  // The slot of the variable `name` names, or nullptr when it has none: a
  // name the body does not declare, a variable of a scope emitted as
  // written, a named fn's own name inside it.
  const Slot* slot(const peg::Ast& name) const {
    auto it = res_.uses.find(&name);
    if (it == res_.uses.end() || it->second.symbol == resolve::kNone)
      return nullptr;
    const Slot& s = slots_[it->second.symbol];
    return s.field.empty() || own_name(it->second) ? nullptr : &s;
  }

  // Whether `target`, a name written to, is a slotted variable that refuses
  // the store: the declaration written last before it made the variable
  // immutable, as the compiler has it for a local. A name that declares is
  // no such store, and one with no slot is the compiler's to refuse.
  bool immutable_write(const peg::Ast& target) const {
    auto it = res_.uses.find(&target);
    if (it == res_.uses.end() || it->second.symbol == resolve::kNone ||
        declaring_.contains(&target))
      return false;
    size_t sym = it->second.symbol;
    if (slots_[sym].field.empty() || own_name(it->second)) return false;
    const auto& ds = declared_[sym];
    // A parameter is declared by its list, ahead of everything.
    std::optional<bool> mut;
    if (res_.symbols[sym].declared_as(resolve::Form::Parameter))
      mut = mut_param_[sym];
    for (const auto& d : ds)
      if (d.at < target.token.data()) mut = d.mut;
    // A store written above every declaration (in a loop, in a closure made
    // first) is refused when no declaration lets it through.
    if (!mut)
      mut = std::any_of(ds.begin(), ds.end(),
                        [](const auto& d) { return d.mut; });
    return !*mut;
  }

  // Whether `name` names a variable of the body's own, slotted or not.
  bool declares(const peg::Ast& name) const {
    size_t sym = res_.symbol_of(name);
    return sym != resolve::kNone && res_.frame_of(sym) == body_;
  }

  // Source giving each boxed variable of `scope` a fresh box, for a scope
  // entered again: a closure made on an earlier pass keeps that pass's
  // variable. Empty when nothing there is boxed.
  std::string fresh_boxes(size_t scope) const {
    std::string out;
    for (const auto& [name, sym] : res_.scopes[scope].names)
      if (!slots_[sym].box.empty())
        out += std::format("      self.{} = {}\n", slots_[sym].field,
                           box_literal("nil"));
    return out;
  }

  // Bind the boxes `body` names, for prepending to it. Every emitted method
  // that carries body source needs this — the box is reached as a plain
  // local, so without the binding the lowered source names a free one.
  // Taking the body rather than a flag is what keeps that rule from being
  // re-decided per method: a method that mentions no box gets no prologue,
  // and one that mentions a box cannot be given the wrong set.
  std::string box_prologue(std::string_view body) const {
    std::string out;
    for (size_t sym : order_) {
      const Slot& s = slots_[sym];
      if (s.box.empty() || body.find(s.box) == std::string_view::npos) continue;
      out += std::format("      let {} = self.{}\n", s.box, s.field);
    }
    return out;
  }

  // The state class's `new`: its slot list `_p0, _p1, ...` for `params`, the
  // arguments its call passes, and after `ctor_inits` the initializer of
  // every slot the body got: a parameter's from its `_pN`, any other nil. A
  // boxed slot holds the box, made once here; every later store goes into
  // its `v` field, so the box the closures captured stays the live one.
  void emit_ctor(std::span<const std::string_view> params,
                 std::string& ctor_params, std::string& ctor_call_args,
                 std::string& ctor_inits) const {
    std::vector<std::string> value(slots_.size(), "nil");
    for (size_t j = 0; j < params.size(); j++) {
      auto arg = std::format("_p{}", j);
      if (j > 0) {
        ctor_params += ", ";
        ctor_call_args += ", ";
      }
      ctor_params += arg;
      ctor_call_args += std::string(params[j]);
      const auto& names = res_.scopes[body_].names;
      if (auto it = names.find(params[j]); it != names.end())
        value[it->second] = arg;
    }
    for (size_t sym : order_) {
      const Slot& s = slots_[sym];
      ctor_inits += std::format(
          "      self.{} = {}\n", s.field,
          s.box.empty() ? value[sym] : box_literal(value[sym]));
    }
  }

  // Source giving every box a copy of its own: a forked frame shares the
  // boxes of the one it was copied from otherwise.
  std::string refork() const {
    std::string out;
    for (size_t sym : order_) {
      const Slot& s = slots_[sym];
      if (!s.box.empty())
        out += std::format("      self.{0} = {1}\n", s.field,
                           box_literal("self." + s.field + ".v"));
    }
    return out;
  }

  // Whether `n`, on the level of `scope`, holds a `defer` of that scope: one
  // reached through no scope of its own. A defer anywhere deeper sits in a
  // scope the machine enters by itself, or in one that runs start to finish
  // inside one state, where the backends' own defer does the work.
  bool has_defer_of(const peg::Ast& n, size_t scope) const {
    using namespace peg::udl;
    if (n.tag == "DEFER"_) {
      if (n.nodes.empty()) return false;
      auto it = res_.body_scope.find(n.nodes[0].get());
      return it != res_.body_scope.end() &&
             res_.scopes[it->second].parent == scope;
    }
    if (is_fn_boundary(n.tag)) return false;
    // Nothing under a block of another scope is this scope's.
    if (auto it = res_.block_scope.find(&n);
        it != res_.block_scope.end() && it->second != scope)
      return false;
    for (const auto& c : n.nodes)
      if (has_defer_of(*c, scope)) return true;
    return false;
  }

 private:
  // A named fn's own name inside its body binds to the declaration itself,
  // not to the slot the declaration is stored in.
  bool own_name(const resolve::Use& use) const {
    if (!res_.symbols[use.symbol].declared_as(resolve::Form::Function))
      return false;
    for (size_t s = use.scope; s != resolve::kNone; s = res_.scopes[s].parent)
      if (res_.scopes[s].owner == use.symbol) return true;
    return false;
  }

  resolve::Resolution res_;
  size_t body_ = resolve::kNone;
  std::vector<Slot> slots_;     // by symbol
  std::vector<char> captured_;  // by symbol: some closure names it
  // By symbol, its declarations in the order they are written: where, and
  // whether one leaves the variable reassignable. A parameter's is its list.
  struct Declared {
    const char* at;
    bool mut;
  };
  std::vector<std::vector<Declared>> declared_;
  std::vector<char> mut_param_;
  std::set<const peg::Ast*> declaring_;  // the name nodes that declare
  std::vector<size_t> order_;   // the slotted symbols, as they got one
  std::map<std::string, int, std::less<>> spelled_;  // slotted per name
};

// `(a, b, _)`: every leaf a plain name — the one destructuring shape that
// also reads as a PLACE_ASSIGN once its leaves are spelled `self.<name>`.
inline bool is_flat_tuple_pattern(const peg::Ast& pattern) {
  using namespace peg::udl;
  if (pattern.tag != "TUPLE_PATTERN"_) return false;
  for (const auto& leaf : pattern.nodes) {
    if (leaf->tag != "IDENTIFIER"_) return false;
  }
  return true;
}

// Where a node sits, as far as appending to it goes: a statement can be
// followed by `; <stmt>`, an init-clause binding by `, <binding>`, and an
// expression by nothing.
enum class EditSite { Expr, Stmt, InitBinding };

// The plain local a promoted leaf of a destructuring pattern binds first, to
// be copied into its slot (only a flat tuple can bind slots directly). Named
// for the pattern's position as well, so two destructurings in one scope each
// declare their own: the binding is a declaration, which a second
// destructuring of the same leaf would otherwise repeat.
inline std::string destructure_temp(const peg::Ast& pat, std::string_view name) {
  return std::format("_g_d{}_{}", pat.position, name);
}

// Bind `pat`'s promoted leaves to temporaries (destructure_temp): push the
// edits that rename them, and return the copies into their slots, each after
// `sep`.
inline MappedSource rename_pattern_leaves(const peg::Ast& pat,
                                         const std::string& src,
                                         const PromotedLocals& promoted,
                                         std::vector<SourceEdit>& out,
                                         std::string_view sep) {
  MappedSource copies;
  for_each_pattern_leaf(pat, [&](const peg::Ast& id, bool shorthand) {
    const auto* slot = promoted.slot(id);
    if (!slot) return;
    auto name = std::string(id.token);
    auto temp = destructure_temp(pat, name);
    auto pos = static_cast<size_t>(id.token.data() - src.data());
    out.push_back({pos, id.token.size(),
                   shorthand ? std::format("{}: {}", name, temp) : temp});
    // A copy that fails (a slot that cannot take the value) is the leaf's.
    copies += std::format("{} ", sep);
    copies += MappedSource::standing_for(
        std::format("{} = {}", slot->place(), temp), src, pos);
  });
  return copies;
}

// The byte of `src` where `node` was written: its line and column, not its
// position (a collapsed node keeps its parent's position but takes its
// child's column).
inline size_t written_at(const peg::Ast& node, const std::string& src) {
  return offset_at(src, node.line, node.column).value_or(node.position);
}

// `text` a lowering writes for `node`, standing where the node was written:
// whatever it raises reports there, as diagnostics do.
inline MappedSource standing_at(std::string text, const peg::Ast& node,
                                const std::string& src) {
  return MappedSource::standing_for(std::move(text), src, written_at(node, src));
}

// The byte just past where `node` itself was written, for an edit that
// replaces it. A node a block collapsed onto keeps the block's range, braces
// and all, so it ends where the last of its children does: a child's range is
// its own (a synthesized one, unless's `!`, has none, so its children answer).
inline size_t written_end(const peg::Ast& node) {
  if (node.nodes.empty()) return node.position + node.length;
  size_t e = 0;
  for (auto& c : node.nodes)
    e = std::max(e, c->length ? c->position + c->length : written_end(*c));
  return e;
}

// Defined below; collect_promoted_edits writes a condition through it.
inline MappedSource rewrite_locals_to_self(const peg::Ast& n,
                                           const std::string& src,
                                           const PromotedLocals& promoted,
                                           EditSite site = EditSite::Expr,
                                           std::vector<SourceEdit> edits = {});

// The edits that move every promoted local under `n` to where it lives
// (PromotedLocals::Slot::place). Only references are touched — a member name (`o.x`) and a
// label (`{x: v}`, `f(x: v)`) are not the local — and a shorthand `{x}` is
// both, so it keeps its key and gains the value (`{x: <slot>}`). A `let` /
// `mut` in front of a promoted target goes: that declaration is now a store
// into a slot the ctor made.
//
// Offsets come from the token views, not from `position`: a node the
// AstOptimizer collapsed into its lone child keeps the parent's span (`{ x }`
// read as an IDENTIFIER spans the braces). They index the one buffer `n` was
// parsed from, so every node below must come from that parse. A lowering
// always rewrites a body before any nested construct in it is lowered
// (EffectsLowerer::transform is top-down), so a subtree spliced in from
// another fragment here is a broken invariant, not a shape to tolerate.
//
// Any other destructuring pattern binds its promoted leaves to temporaries
// (destructure_temp) and is followed by the copies into their slots, so the
// pattern keeps its own matching and errors. That needs somewhere to put the
// copies: a statement (one with a trailing `if` / `unless` is wrapped in the
// `if` it means) or an init-clause binding.
inline void collect_promoted_edits(const peg::Ast& n, const std::string& src,
                                   const PromotedLocals& promoted,
                                   std::vector<SourceEdit>& out,
                                   EditSite site) {
  using namespace peg::udl;
  auto at = [&](const peg::Ast& t) {
    return static_cast<size_t>(t.token.data() - src.data());
  };
  auto is_promoted = [&](const peg::Ast& id) {
    return id.tag == "IDENTIFIER"_ && id.is_token && promoted.slot(id);
  };
  auto slot = [&](const peg::Ast& id) { return promoted.slot(id)->place(); };
  // The `let` / `mut` keyword(s) of a declaration, up to its first target.
  auto drop_keywords = [&](const peg::Ast& decl, size_t target) {
    const auto& kw = decl.nodes[0]->token.empty() ? *decl.nodes[1]
                                                  : *decl.nodes[0];
    out.push_back({at(kw), target - at(kw), ""});
  };
  // A store a slotted variable refuses (PromotedLocals::immutable_write) is
  // left to the compiler. The store `stmt` runs in a block where each slotted
  // name among `targets` is a local of that name again: a `let` for one that
  // refuses, so the store raises as in any fn (after the right-hand side, at
  // the target, under the same message), a `mut` copied back on the way out
  // for one that takes it. Where the store is an expression the block is an
  // `if`'s arm, which has its value. Called once the edits under `stmt` are
  // in: a store in its right-hand side ends where it does, and closes first.
  auto as_locals = [&](const peg::Ast& stmt,
                       const std::vector<const peg::Ast*>& targets,
                       const peg::Ast& rhs) {
    bool is_stmt = site == EditSite::Stmt;
    std::string head = is_stmt ? "{ " : "(if true { ";
    std::set<const PromotedLocals::Slot*> seen;
    for (const auto* t : targets) {
      const auto* s = promoted.slot(*t);
      if (!s || !seen.insert(s).second) continue;
      head += promoted.immutable_write(*t)
                  ? std::format("let {} = {}; ", t->token, s->place())
                  : std::format("mut {0} = {1}; defer {{ {1} = {0} }}; ",
                                t->token, s->place());
    }
    out.push_back({written_at(stmt, src), 0, head});
    out.push_back({rhs.position + rhs.length, 0, is_stmt ? " }" : " })"});
  };
  auto refuses = [&](const std::vector<const peg::Ast*>& targets) {
    return std::any_of(targets.begin(), targets.end(), [&](const peg::Ast* t) {
      return promoted.immutable_write(*t);
    });
  };
  // Everything under `stmt` but `targets` moves to its slot.
  auto rewrite_but = [&](const peg::Ast& stmt,
                         const std::vector<const peg::Ast*>& targets) {
    for (const auto& c : stmt.nodes)
      if (std::find(targets.begin(), targets.end(), c.get()) == targets.end())
        collect_promoted_edits(*c, src, promoted, out, EditSite::Expr);
  };
  if (n.tag == "IDENTIFIER"_) {
    if (n.original_tag != "DOT"_ && n.original_tag != "SAFE_DOT"_ &&
        is_promoted(n))
      out.push_back({at(n), n.token.size(), slot(n)});
    return;
  }
  if (n.tag == "ASSIGNMENT"_) {
    auto av = view_assignment(n);
    const auto* t = assign_name_target(n, av);
    if ((av.is_let || av.is_mut) && t && is_promoted(*t)) {
      drop_keywords(n, at(*t));
    } else if (t && refuses({t})) {
      rewrite_but(n, {t});
      as_locals(n, {t}, *av.rhs);
      return;
    }
  } else if (n.tag == "PLACE_ASSIGN"_) {
    std::vector<const peg::Ast*> names;
    for_each_place_target(
        n, [](const peg::Ast&) {},
        [&](const peg::Ast& name) { names.push_back(&name); });
    if (refuses(names)) {
      rewrite_but(n, names);
      as_locals(n, names, *n.nodes.back());
      return;
    }
  } else if (n.tag == "DESTRUCTURE_ASSIGN"_) {
    auto dv = view_destructure(n);
    const auto& pat = *dv.pattern;
    bool any = false;
    std::vector<const peg::Ast*> leaves;
    for_each_pattern_leaf(pat, [&](const peg::Ast& id, bool) {
      any = any || is_promoted(id);
      leaves.push_back(&id);
    });
    if (refuses(leaves)) {
      collect_promoted_edits(*dv.rhs, src, promoted, out, EditSite::Expr);
      as_locals(n, leaves, *dv.rhs);
      return;
    }
    if (any && is_flat_tuple_pattern(pat)) {
      // Dropping the keyword leaves the PLACE_ASSIGN `(<slot>, <slot>) = …`.
      if (dv.declares) drop_keywords(n, pat.position);
    } else if (any) {
      if (site == EditSite::Expr) {
        auto p = source_pos(pat, src);
        throw CulebraError(
            "SyntaxError",
            "a destructuring assignment that binds a local of an effect body "
            "must be a statement of its own.",
            p.line, p.col);
      }
      auto copies = rename_pattern_leaves(
          pat, src, promoted, out, site == EditSite::Stmt ? ";" : ",");
      out.push_back({dv.rhs->position + dv.rhs->length, 0, copies});
      collect_promoted_edits(*dv.rhs, src, promoted, out, EditSite::Expr);
      return;
    }
  } else if (n.tag == "OBJECT_PROPERTY"_ && n.nodes.size() == 2 &&
             is_promoted(*n.nodes[1])) {
    const auto& key = *n.nodes[1];
    out.push_back({at(key) + key.token.size(), 0, ": " + slot(key)});
    return;
  } else if (auto pv = view_postfix_if(n);
             pv && pv->stmt->tag == "DESTRUCTURE_ASSIGN"_) {
    // `<destructure> if c`, whose source still reads that way: the copies
    // must stay under the condition, so it is written as the IF it is.
    const auto& base = *pv->stmt;
    std::vector<SourceEdit> inner;
    collect_promoted_edits(base, src, promoted, inner, EditSite::Stmt);
    if (!inner.empty()) {
      out.push_back({base.position, 0,
                     "if " + rewrite_locals_to_self(*n.nodes[0], src, promoted) +
                         " { "});
      out.insert(out.end(), inner.begin(), inner.end());
      size_t base_end = base.position + base.length;
      out.push_back({base_end, written_end(n) - base_end, " }"});
      return;
    }
  }
  auto contains = [](unsigned int tag) {
    return tag == "STATEMENTS"_ || tag == "BLOCK"_ || tag == "LEXICAL_SCOPE"_;
  };
  for (size_t i = 0; i < n.nodes.size(); i++) {
    const auto& c = *n.nodes[i];
    if (c.path != n.path) {
      throw CulebraError(
          "InternalError",
          "locals rewrite reached a subtree lowered from another fragment",
          static_cast<long>(c.line), static_cast<long>(c.column));
    }
    if (is_label_position(n, i)) continue;
    // A block collapsed into its lone statement keeps the block's tag in
    // original_tag.
    EditSite cs = EditSite::Expr;
    if (contains(n.tag) || contains(c.original_tag) ||
        c.original_tag == "STATEMENT"_ || (n.tag == "STATEMENT"_ && i == 0))
      cs = EditSite::Stmt;
    else if (n.tag == "INIT_CLAUSE"_)
      cs = EditSite::InitBinding;
    collect_promoted_edits(c, src, promoted, out, cs);
  }
}

// `n`'s source from `src` with every promoted local moved to its slot (see
// collect_promoted_edits). A promoted local stays a plain variable: reading
// one hands back the value and calling one passes no receiver, though both are
// spelled `self.<name>` now — both backends check the slot's owner
// (culebra::is_lowered_state_class), the one place that sees every spelling.
// `site` is where `n` itself sits (see EditSite); `edits` are the caller's
// own, applied along with the rewrite (ahead of any at the same offset).
inline MappedSource rewrite_locals_to_self(const peg::Ast& n,
                                           const std::string& src,
                                           const PromotedLocals& promoted,
                                           EditSite site,
                                           std::vector<SourceEdit> edits) {
  using namespace peg::udl;
  // The `!c` that `unless c` runs has no source of its own (see
  // make_postfix_if): it is written around `c`, standing where the
  // synthesized `!` does.
  if (n.original_tag == "STMT_MODIFIER_UNLESS"_) {
    const auto& c = *n.nodes[1];
    return standing_at("!(", n, src) +
           rewrite_locals_to_self(c, src, promoted, EditSite::Expr,
                                  std::move(edits)) +
           ")";
  }
  if (ast_source_slice(n, src).empty()) return {};
  collect_promoted_edits(n, src, promoted, edits, site);
  return rewrite_edits(n, src, std::move(edits));
}

// The [begin, end) offsets of a block's statements in `src`: its span inside
// the braces, or the whole span of a node without them (a STATEMENTS list).
inline std::pair<size_t, size_t> block_inner_span(const peg::Ast& block,
                                                  const std::string& src) {
  auto whole = ast_source_slice(block, src);
  size_t trim = whole.size() >= 2 && whole.front() == '{' && whole.back() == '}';
  return {block.position + trim, block.position + block.length - trim};
}

// The same over a block's statements: its source inside the braces.
inline MappedSource rewrite_block_inner(const peg::Ast& block,
                                        const std::string& src,
                                        const PromotedLocals& promoted,
                                        std::vector<SourceEdit> edits = {}) {
  collect_promoted_edits(block, src, promoted, edits, EditSite::Stmt);
  auto [begin, end] = block_inner_span(block, src);
  return splice_source(src, begin, end, std::move(edits),
                       static_cast<long>(block.line),
                       static_cast<long>(block.column));
}

// Positional parameter names of a PARAMETERS node, skipping the kw-only
// separator and any `**kwargs` rest: the names ctor slot emission is fed.
inline std::vector<std::string_view> collect_positional_param_names(
    const peg::Ast& params_ast) {
  std::vector<std::string_view> names;
  for (const auto& pn : params_ast.nodes) {
    if (is_kw_only_sep(*pn) || is_kwargs_rest(*pn)) continue;
    names.push_back(view_parameter(*pn).name);
  }
  return names;
}

// The parameters of a PARAMETERS node declared `mut`.
inline std::vector<std::string_view> collect_mut_param_names(
    const peg::Ast& params_ast) {
  std::vector<std::string_view> names;
  for (const auto& pn : params_ast.nodes) {
    if (is_kw_only_sep(*pn) || is_kwargs_rest(*pn)) continue;
    if (auto pv = view_parameter(*pn); pv.is_mut) names.push_back(pv.name);
  }
  return names;
}

// Where a lowering walks: the root, the nodes from it down to the one being
// walked, and the names visible around the root. A body to lower is resolved
// under the names visible where it stands (PromotedLocals), which the root's
// own resolution says; the root is resolved the first time one is asked for,
// so a program with no effect pays nothing.
struct Surroundings {
  const peg::Ast* root = nullptr;
  std::vector<std::string> around;
  std::vector<const peg::Ast*> path;
  // The root's resolution, shared by the copies of one walk (a lowerer handed
  // a subtree of another buffer walks on with a copy).
  std::shared_ptr<std::optional<resolve::Resolution>> resolved =
      std::make_shared<std::optional<resolve::Resolution>>();

  // `ast`'s children, each replaced by `f` of it, with `ast` on the path
  // meanwhile.
  template <typename F>
  void walk_children(peg::Ast& ast, F&& f) {
    path.push_back(&ast);
    for (auto& c : ast.nodes) c = f(c);
    path.pop_back();
  }

  // The names visible where the walk stands: those of the innermost scope
  // the path is in, and of every scope around it.
  std::vector<std::string> visible() {
    if (!*resolved)
      resolved->emplace(
          resolve::resolve_module(*root, {}, lint::resolve_options(around)));
    const resolve::Resolution* res = &**resolved;
    size_t scope = 0;
    for (size_t i = path.size(); i-- > 0;) {
      auto it = res->body_scope.find(path[i]);
      if (it == res->body_scope.end()) {
        it = res->block_scope.find(path[i]);
        if (it == res->block_scope.end()) continue;
      }
      scope = it->second;
      break;
    }
    std::vector<std::string> out;
    for (size_t s = scope; s != resolve::kNone; s = res->scopes[s].parent)
      for (const auto& [name, sym] : res->scopes[s].names) out.push_back(name);
    return out;
  }
};

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

// Re-parse a `fn __wrapper__(...) { ... }` source fragment (registered for
// lifetime, see parse_registered_source) under `label` and return its
// MULTIFN_DECL.
inline std::shared_ptr<peg::Ast> parse_wrapper_fn(
    std::shared_ptr<std::string> synthesized, const char* label) {
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

// True if `node` contains a break/continue that targets an *enclosing*
// loop (i.e. not nested inside a while/for within `node`), or a return
// anywhere. Such statements can't be emitted verbatim into the dispatch
// loop — break/continue/return must become state jumps. A self-contained
// inner loop with its own internal break stays verbatim-safe (the break
// binds to that real inner loop). Stops at fn boundaries.
//
// `open` is the labels of the loops within `node` that enclose the walk,
// innermost last (empty for an unlabelled one), so a labelled break escapes
// exactly when no loop inside `node` carries its label — `break outer` inside
// a nested loop is escaping even though its depth is not 0.
inline bool escaping_loop_ctrl(const peg::Ast& node,
                               std::vector<std::string_view>& open) {
  using namespace peg::udl;
  if (is_fn_boundary(node.tag)) return false;
  if (node.tag == "RETURN"_) return true;
  if (node.tag == "BREAK"_ || node.tag == "CONTINUE"_) {
    auto label = culebra::break_label_of(node);
    if (label.empty()) return open.empty();
    return std::find(open.begin(), open.end(), label) == open.end();
  }
  bool is_loop = node.tag == "WHILE"_ || node.tag == "FOR"_;
  const peg::Ast* label = is_loop ? culebra::loop_label_of(node) : nullptr;
  for (auto& c : node.nodes) {
    // A loop body is one level deeper (its break/continue are bound here); but
    // a trailing `nobreak { … }` runs *outside* the loop, so its break/continue
    // escape to an enclosing loop — walk it at the current depth.
    if (!is_loop || c->tag == "NOBREAK_CLAUSE"_) {
      if (escaping_loop_ctrl(*c, open)) return true;
      continue;
    }
    open.push_back(label ? label->token : std::string_view{});
    bool escapes = escaping_loop_ctrl(*c, open);
    open.pop_back();
    if (escapes) return true;
  }
  return false;
}

inline bool has_escaping_loop_ctrl(const peg::Ast& node) {
  std::vector<std::string_view> open;
  return escaping_loop_ctrl(node, open);
}

// One enclosing CPS-managed loop: its header and exit states, plus the label
// it carries (empty when unlabelled).
struct CpsLoop {
  int header;
  int exit;
  std::string label;
};

// The enclosing loop a break/continue targets, innermost last, or nullptr
// when its label names none (which `has_escaping_loop_ctrl` should already
// have kept out of a CPS-managed body — failing closed rather than
// mis-jumping).
inline const CpsLoop* target_loop(const std::vector<CpsLoop>& stack,
                                  const peg::Ast& node) {
  if (stack.empty()) return nullptr;
  auto label = culebra::break_label_of(node);
  if (label.empty()) return &stack.back();
  for (auto it = stack.rbegin(); it != stack.rend(); ++it)
    if (it->label == label) return &*it;
  return nullptr;
}

}  // namespace culebra
