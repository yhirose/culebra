// The source a lowering synthesizes, and where its text came from.
//
// The generator and effects transforms rewrite a body's source text and
// re-parse it, so the re-parsed AST's positions index a synthesized buffer,
// not the user's file. Every run of user code they copy into one is recorded
// (MappedSource), and a copy is placed into machinery text only behind an
// anchor comment (`anchored`) naming that record. After the final fragment
// parse, SourceResolver reads the anchors back and follows them — through the
// intermediate buffers a lowering re-parses on the way — to the byte of the
// user's file a node came from.

#pragma once

#include "frontend/parser.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace culebra {

// --- String literals -----------------------------------------------------

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

// --- Where a copied byte came from ---------------------------------------

// Bytes [at, at + len) of a MappedSource's text are bytes of `*src` starting
// at `src_pos` — or, for a `point` run (text standing in for a user token,
// like the slot a promoted local is rewritten to), all of them stand for the
// one byte at `src_pos`.
struct SourceRun {
  size_t at;
  size_t len;
  const std::string* src;
  size_t src_pos;
  bool point = false;
};

// Text a lowering emits, with the runs of it that are user code. Machinery
// text carries no run. Composing two keeps both sets, shifted, so a piece
// built up out of other pieces still maps every byte it copied.
class MappedSource {
 public:
  MappedSource() = default;
  MappedSource(std::string machinery) : text_(std::move(machinery)) {}
  MappedSource(const char* machinery) : text_(machinery) {}

  // Bytes [pos, pos + len) of `src`, verbatim.
  static MappedSource copy_of(const std::string& src, size_t pos, size_t len) {
    MappedSource m(src.substr(pos, len));
    if (len) m.runs_.push_back({0, len, &src, pos});
    return m;
  }
  // `text` standing for the token at `src_pos` of `src`.
  static MappedSource standing_for(std::string text, const std::string& src,
                                   size_t src_pos) {
    MappedSource m(std::move(text));
    if (!m.text_.empty())
      m.runs_.push_back({0, m.text_.size(), &src, src_pos, true});
    return m;
  }

  MappedSource& operator+=(std::string_view machinery) {
    text_ += machinery;
    return *this;
  }
  MappedSource& operator+=(const std::string& machinery) {
    return *this += std::string_view(machinery);
  }
  MappedSource& operator+=(const char* machinery) {
    return *this += std::string_view(machinery);
  }
  MappedSource& operator+=(const MappedSource& m) {
    for (auto r : m.runs_) {
      r.at += text_.size();
      runs_.push_back(r);
    }
    text_ += m.text_;
    return *this;
  }
  friend MappedSource operator+(MappedSource a, const MappedSource& b) {
    a += b;
    return a;
  }

  const std::string& text() const { return text_; }
  const std::vector<SourceRun>& runs() const { return runs_; }
  bool empty() const { return text_.empty(); }

 private:
  std::string text_;
  std::vector<SourceRun> runs_;
};

// One splice into a buffer: `len` bytes at offset `pos` become `text`.
struct SourceEdit {
  size_t pos;
  size_t len;
  MappedSource text;
};

// --- Fragment storage ----------------------------------------------------

// peg::Ast holds `string_view`s into the parsed source, so anything a
// transform re-parses needs backing that outlives the AST: the process by
// default — the same fix the lazy-module path uses — or a caller-owned
// FragmentLedger.
//
// Process-global, and written from several threads at once: every isolate
// resolves the lazy stdlib modules on its own thread and those modules contain
// generators, so N children can be inside the transform together. (It cannot
// be thread_local either — the builtin-traits preamble is parsed once and its
// AST is shared by every thread, so the buffers it views must outlive the
// thread that made them.) Hence the lock, and hence accessors that do the work
// rather than hand out a reference a caller could touch unlocked: two
// unsynchronised push_backs reallocating at once free the same buffer twice.
// Never held across `parse`, which re-enters here for nested lowering.
inline std::mutex& fragment_ledger_mutex() {
  static std::mutex m;
  return m;
}

// What one anchor names: its text's length and runs (MappedSource).
struct SourceAnchor {
  size_t len;
  std::vector<SourceRun> runs;
};

// The synthesized buffers a lowering's AST views, and the parse label each was
// read under. `by_label` lets a transform that walks a tree with spliced-in
// subtrees (identified by a different `node->path`) resolve the right slice
// base — e.g. the effects pass reaching a construct inside a generator-lowered
// body. Non-unique labels (internal re-parses that never splice nodes into the
// final AST) may overwrite each other; only `next_fragment_label` labels are
// ever looked up. `anchors` are the records `anchored` wrote, by id; their runs
// point into buffers this ledger holds or into the user's own source.
struct FragmentLedger {
  std::vector<std::shared_ptr<std::string>> sources;
  std::map<std::string, std::shared_ptr<std::string>, std::less<>> by_label;
  std::unordered_set<const std::string*> held;
  std::deque<SourceAnchor> anchors;
};

// The default owner, for every AST that may be cached or shared across threads.
inline FragmentLedger& _process_fragment_ledger() {
  static FragmentLedger ledger;
  return ledger;
}

// Non-null only for the span of one `parse_with_transforms(..., FragmentLedger&)`
// call, so a caller that owns its ASTs frees their fragments with them (an
// editor re-analysing a buffer on every keystroke). Nothing but that one
// lowering runs inside the span, so no process-lifetime AST can come to view a
// buffer this ledger frees.
inline thread_local FragmentLedger* _scoped_fragment_ledger = nullptr;

inline FragmentLedger& _active_fragment_ledger() {
  return _scoped_fragment_ledger ? *_scoped_fragment_ledger
                                 : _process_fragment_ledger();
}

// The buffer a spliced-in subtree was parsed from, or null. A copy of the
// shared_ptr, so the caller reads the buffer without holding the lock.
inline std::shared_ptr<std::string> fragment_source_for(
    std::string_view label) {
  std::lock_guard<std::mutex> lk(fragment_ledger_mutex());
  auto& reg = _active_fragment_ledger().by_label;
  auto it = reg.find(label);
  return it == reg.end() ? nullptr : it->second;
}

// Every fragment the active ledger holds (CULEBRA_TRANSFORM_STATS reporting).
inline std::vector<std::shared_ptr<std::string>> fragment_sources_snapshot() {
  std::lock_guard<std::mutex> lk(fragment_ledger_mutex());
  return _active_fragment_ledger().sources;
}

// Parse a synthesized buffer after registering it (the resulting AST's
// string_views point into it). The single place that pairs the lifetime store
// with `parse` — used by the generator wrapper parse and the effects
// transform's re-parses alike, so the "register before parse" rule lives in
// one spot.
inline std::shared_ptr<peg::Ast> parse_registered_source(
    const char* label, std::shared_ptr<std::string> synthesized) {
  {
    std::lock_guard<std::mutex> lk(fragment_ledger_mutex());
    auto& ledger = _active_fragment_ledger();
    ledger.sources.push_back(synthesized);
    ledger.by_label[label] = synthesized;
    ledger.held.insert(synthesized.get());
  }
  std::vector<std::string> msgs;
  return parse(label, *synthesized, msgs);
}

// --- Anchors ---------------------------------------------------------------

// `/*@culebra:<id>*/`: a block comment, so it may sit wherever whitespace may —
// which is NOT everywhere inside an expression (`-x` admits none after the
// minus). Hence the one rule: `anchored` output is placed where a statement,
// a parenthesized expression or a binding's value starts, and pieces are
// composed as MappedSource below that.
inline constexpr std::string_view kSourceAnchor = "/*@culebra:";

// `m`'s text headed by an anchor naming its runs.
inline std::string anchored(const MappedSource& m) {
  if (m.runs().empty()) return m.text();
  size_t id;
  {
    std::lock_guard<std::mutex> lk(fragment_ledger_mutex());
    auto& anchors = _active_fragment_ledger().anchors;
    id = anchors.size();
    anchors.push_back({m.text().size(), m.runs()});
  }
  return std::format("{}{}*/{}", kSourceAnchor, id, m.text());
}

// An anchor found in a buffer: the comment's span, and the anchor's id.
struct FoundAnchor {
  size_t begin;  // the comment's first byte
  size_t end;    // just past it: where the named text starts
  size_t id;
};

// The anchors in `s`, in order. Only code is searched: a string literal (holes
// and all — no anchor is ever placed in one) and a line comment are skipped,
// so user text that merely spells an anchor is left alone.
inline std::vector<FoundAnchor> find_anchors(std::string_view s) {
  std::vector<FoundAnchor> out;
  if (s.find(kSourceAnchor) == std::string_view::npos) return out;
  size_t i = 0, n = s.size();
  while (i < n) {
    char c = s[i];
    if (c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/')) {
      while (i < n && s[i] != '\n') i++;
    } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      size_t close = s.find("*/", i + 2);
      size_t end = close == std::string_view::npos ? n : close + 2;
      if (s.substr(i, kSourceAnchor.size()) == kSourceAnchor &&
          close != std::string_view::npos) {
        auto digits = s.substr(i + kSourceAnchor.size(),
                               close - i - kSourceAnchor.size());
        size_t id = 0;
        auto [p, ec] =
            std::from_chars(digits.data(), digits.data() + digits.size(), id);
        if (ec == std::errc{} && p == digits.data() + digits.size())
          out.push_back({i, end, id});
      }
      i = end;
    } else if (c == '"' || c == '\'' || c == '`') {
      i = skip_string_literal(s, i);
    } else {
      i++;
    }
  }
  return out;
}

// Bytes [begin, end) of `src` with `edits` applied (each inside the range,
// none overlapping), mapped back to `src`. An anchor inside the range is
// dropped: `src` keeps it, and a byte copied from after it resolves through
// it there. Text an edit puts in place of user bytes stands for the first of
// them (a promoted local's slot, for the name it replaced).
inline MappedSource splice_source(const std::string& src, size_t begin,
                                  size_t end, std::vector<SourceEdit> edits,
                                  long err_line = 0, long err_col = 0) {
  for (const auto& a : find_anchors(std::string_view(src).substr(begin, end - begin)))
    edits.push_back({begin + a.begin, a.end - a.begin, {}});
  std::stable_sort(edits.begin(), edits.end(),
                   [](const auto& a, const auto& b) { return a.pos < b.pos; });
  MappedSource out;
  size_t at = begin;
  for (const auto& e : edits) {
    if (e.pos < at || e.pos + e.len > end) {
      throw CulebraError("InternalError",
                         "locals rewrite produced an overlapping edit",
                         err_line, err_col);
    }
    out += MappedSource::copy_of(src, at, e.pos - at);
    if (e.len && e.text.runs().empty())
      out += MappedSource::standing_for(e.text.text(), src, e.pos);
    else
      out += e.text;
    at = e.pos + e.len;
  }
  out += MappedSource::copy_of(src, at, end - at);
  return out;
}

// `n`'s source from `src` with `edits` applied.
inline MappedSource rewrite_edits(const peg::Ast& n, const std::string& src,
                                  std::vector<SourceEdit> edits) {
  return splice_source(src, n.position, n.position + n.length,
                       std::move(edits), static_cast<long>(n.line),
                       static_cast<long>(n.column));
}

// `n`'s source from `src`, verbatim.
inline MappedSource node_source(const peg::Ast& n, const std::string& src) {
  return rewrite_edits(n, src, {});
}

// --- Reading the anchors back --------------------------------------------

struct SourcePos {
  long line;
  long col;
};

// Where a byte of a synthesized buffer came from in the user's source. It
// caches, per buffer it visits, the anchors found in it and its line starts —
// both fixed once the buffer is parsed — so one serves a whole lowering.
class SourceResolver {
 public:
  // The user-source line and column (as the parser counts them: 1-based,
  // column in code points) of byte `off` of `buf`, or nullopt for a byte a
  // lowering synthesized.
  std::optional<SourcePos> resolve(const std::string& buf, size_t off) {
    const std::string* at = &buf;
    // Each hop moves to a buffer made before this one, so the chain ends; the
    // bound only guards against a forged anchor pointing back.
    for (int hops = 0; hops < 64; hops++) {
      auto& b = buffer(*at);
      if (!b.fragment) return position_in(b, *at, off);
      // The anchor whose text covers `off`: the last to start at or before
      // it (anchored text is placed side by side, never inside another's).
      auto a = std::upper_bound(
          b.anchors.begin(), b.anchors.end(), off,
          [](size_t o, const Anchor& x) { return o < x.start; });
      if (a == b.anchors.begin()) return std::nullopt;
      const auto& hit = *std::prev(a);
      if (off >= hit.start + hit.rec->len) return std::nullopt;
      size_t rel = off - hit.start;
      const auto& runs = hit.rec->runs;  // in `at` order, disjoint
      auto r = std::upper_bound(
          runs.begin(), runs.end(), rel,
          [](size_t o, const SourceRun& x) { return o < x.at; });
      if (r == runs.begin() || rel >= std::prev(r)->at + std::prev(r)->len)
        return std::nullopt;
      const auto& run = *std::prev(r);
      off = run.point ? run.src_pos : run.src_pos + (rel - run.at);
      at = run.src;
    }
    return std::nullopt;
  }

  // The same for node `n` parsed from `buf`. By its line and column, not its
  // `position`: a node the AstOptimizer collapsed into its lone child keeps
  // the parent's span but takes the child's line and column (`{ x }` read as
  // `x` starts at the brace), and the column is the one diagnostics report.
  std::optional<SourcePos> resolve(const std::string& buf, const peg::Ast& n) {
    auto& b = buffer(buf);
    index_lines(b, buf);
    if (n.line == 0 || n.line > b.line_starts.size()) return std::nullopt;
    size_t off = b.line_starts[n.line - 1];
    // Past `column - 1` code points, counted as peg::codepoint_count does.
    for (size_t cp = 1; cp < n.column && off < buf.size(); cp++)
      off += std::max<size_t>(
          1, peg::codepoint_length(buf.data() + off, buf.size() - off));
    return resolve(buf, off);
  }

 private:
  // An anchor in a buffer: where its text starts, and its record. The ledger
  // is a deque that only grows, so the record stays put.
  struct Anchor {
    size_t start;
    const SourceAnchor* rec;
  };
  struct Buffer {
    bool fragment = false;
    std::vector<Anchor> anchors;
    std::vector<size_t> line_starts;
  };
  std::map<const std::string*, Buffer> buffers_;

  Buffer& buffer(const std::string& buf) {
    auto [it, fresh] = buffers_.try_emplace(&buf);
    if (!fresh) return it->second;
    auto& b = it->second;
    auto found = find_anchors(buf);
    std::lock_guard<std::mutex> lk(fragment_ledger_mutex());
    auto& ledger = _active_fragment_ledger();
    b.fragment = ledger.held.contains(&buf);
    if (!b.fragment) return b;  // the user's source: nothing to follow
    for (auto& a : found)
      if (a.id < ledger.anchors.size())
        b.anchors.push_back({a.end, &ledger.anchors[a.id]});
    return b;
  }

  static void index_lines(Buffer& b, const std::string& buf) {
    if (!b.line_starts.empty()) return;
    b.line_starts.push_back(0);
    for (size_t i = 0; i < buf.size(); i++)
      if (buf[i] == '\n') b.line_starts.push_back(i + 1);
  }

  static std::optional<SourcePos> position_in(Buffer& b, const std::string& buf,
                                              size_t off) {
    if (off > buf.size()) return std::nullopt;
    index_lines(b, buf);
    auto it = std::upper_bound(b.line_starts.begin(), b.line_starts.end(), off);
    size_t line = static_cast<size_t>(it - b.line_starts.begin());
    size_t start = *(it - 1);
    auto col = peg::codepoint_count(buf.data() + start, off - start) + 1;
    return SourcePos{static_cast<long>(line), static_cast<long>(col)};
  }
};

// Where node `n`, parsed from `src`, sits in the user's source — its own
// position when it has none there (a lowering's machinery, or `src` being
// the user's source already). For a lowering's diagnostics.
inline SourcePos source_pos(const peg::Ast& n, const std::string& src) {
  SourceResolver resolver;
  if (auto p = resolver.resolve(src, n)) return *p;
  return {static_cast<long>(n.line), static_cast<long>(n.column)};
}

}  // namespace culebra
