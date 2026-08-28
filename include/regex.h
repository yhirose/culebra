#pragma once

// Value-neutral regex core for the Regex namespace (the `_Regex` native rows
// in stdlib_jit.h). Mirrors http.h / sqlite.h: no Value / JitValue here — the
// binding layer adapts the byte spans below into its own Match objects.
//
// Linkage partitioning, the http_request / tensor_eval_node choke applied to
// a self-hosted engine: cpp-regexlib links no external library, but it is
// ~320 KB of code that every AOT binary used to carry whether or not the
// program mentions Regex. The functions below are the whole surface the
// binding layer reaches, so they are the choke:
//   - core archive  (CULEBRA_RT_REGEX_WEAK):   weak throwing stubs; regexlib.h
//     is never even included, so the archive references no reg:: symbol.
//   - regex archive (CULEBRA_RT_REGEX_STRONG): strong real bodies, force-loaded
//     only when the AST scan reports Regex use (a `re'...'` literal desugars
//     to a Regex.compile call before the scan, so it counts too).
//   - header-only / in-process JIT (neither): the normal inline body.

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <shared.h>  // CulebraError

#if !defined(CULEBRA_RT_REGEX_WEAK)
#include <regexlib.h>
#else
namespace reg { class Regex; }  // named, never touched, by the stubs below
#endif

#if defined(CULEBRA_RT_REGEX_STRONG)
#define CULEBRA_RT_REGEX_LINKAGE
#elif defined(CULEBRA_RT_REGEX_WEAK)
#define CULEBRA_RT_REGEX_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_REGEX_LINKAGE inline
#endif

namespace culebra::regex {

// One capture group's byte span in the subject. An unmatched (optional)
// group reports matched == false with a zero span.
struct Span {
  size_t begin = 0, end = 0;
  bool matched = false;
  std::string_view text(std::string_view subject) const {
    return subject.substr(begin, end - begin);
  }
};

// Matches as one flat table, `stride` Spans per match (group 0 first, then
// 1..n in pattern order), so a whole find_all is a single allocation.
// search / match / find_at fill zero or one row.
struct Matches {
  std::vector<Span> spans;
  size_t stride = 0;
  size_t size() const { return stride ? spans.size() / stride : 0; }
  std::span<const Span> operator[](size_t i) const {
    return {spans.data() + i * stride, stride};
  }
};

// One step of a positional scan (find_at): the leftmost match at or after
// `pos`, and where the next step resumes (past an empty match by one match
// unit; text.size() + 1 once no scan position remains).
struct FindAt {
  Matches m;
  size_t next_pos = 0;
};

// A compiled pattern, owned by the per-thread compile cache and handed out
// shared so a caller's handle outlives a cache eviction.
using Compiled = reg::Regex;
using Handle = std::shared_ptr<const Compiled>;

#if defined(CULEBRA_RT_REGEX_WEAK)

[[noreturn]] inline void _not_linked() {
  throw CulebraError("InternalError",
                     "Regex runtime entered in a no-Regex binary", 0, 0);
}
CULEBRA_RT_REGEX_LINKAGE Handle compile(std::string_view) { _not_linked(); }
CULEBRA_RT_REGEX_LINKAGE const std::unordered_map<std::string, int>&
named_groups(const Compiled&) { _not_linked(); }
CULEBRA_RT_REGEX_LINKAGE bool test(const Compiled&, std::string_view) {
  _not_linked();
}
CULEBRA_RT_REGEX_LINKAGE Matches search(const Compiled&, std::string_view) {
  _not_linked();
}
CULEBRA_RT_REGEX_LINKAGE Matches match(const Compiled&, std::string_view) {
  _not_linked();
}
CULEBRA_RT_REGEX_LINKAGE Matches find_all(const Compiled&, std::string_view) {
  _not_linked();
}
CULEBRA_RT_REGEX_LINKAGE std::vector<Span> find_all_spans(const Compiled&,
                                                          std::string_view) {
  _not_linked();
}
CULEBRA_RT_REGEX_LINKAGE size_t count(const Compiled&, std::string_view) {
  _not_linked();
}
CULEBRA_RT_REGEX_LINKAGE FindAt find_at(const Compiled&, std::string_view,
                                        size_t) {
  _not_linked();
}
CULEBRA_RT_REGEX_LINKAGE std::string replace_all(const Compiled&,
                                                 std::string_view,
                                                 std::string_view) {
  _not_linked();
}
CULEBRA_RT_REGEX_LINKAGE std::string replace_first(const Compiled&,
                                                   std::string_view,
                                                   std::string_view) {
  _not_linked();
}

#else  // the real bodies

// Compile (or cache-hit) `pattern`. Flags are written inline in the pattern
// ((?i)/(?m)/(?s)), so it is the cache key. Throws CulebraError("RegexError")
// for a malformed pattern, with no position (the binding layer stamps one).
CULEBRA_RT_REGEX_LINKAGE Handle compile(std::string_view pattern) {
  static thread_local std::unordered_map<std::string, Handle> cache;
  std::string p(pattern);
  auto it = cache.find(p);
  if (it != cache.end()) return it->second;
  Handle h;
  try {
    h = std::make_shared<const Compiled>(p);
  } catch (const reg::RegexError& e) {
    throw CulebraError("RegexError", culebra::format("Regex: {}", e.what()), 0, 0);
  }
  if (cache.size() > 256) cache.clear();  // bound growth
  cache.emplace(std::move(p), h);
  return h;
}

CULEBRA_RT_REGEX_LINKAGE const std::unordered_map<std::string, int>&
named_groups(const Compiled& re) {
  return re.named_groups();
}

// Append one row for an engine match (`reg::MatchResult` owning / `reg::Match`
// view — both answer group(i) with absolute byte offsets).
template <typename MatchT>
inline void _push_row(Matches& out, const MatchT& m) {
  for (size_t i = 0; i < out.stride; i++) {
    auto g = m.group(i);
    out.spans.push_back({g.begin(), g.end(), g.matched()});
  }
}
template <typename MatchT>
inline Matches _one_row(const Compiled& re, const MatchT& m) {
  Matches out;
  out.stride = re.group_count() + 1;
  if (m.matched()) _push_row(out, m);
  return out;
}

CULEBRA_RT_REGEX_LINKAGE bool test(const Compiled& re, std::string_view s) {
  return re.test(s);
}
CULEBRA_RT_REGEX_LINKAGE Matches search(const Compiled& re, std::string_view s) {
  return _one_row(re, re.search(s));
}
CULEBRA_RT_REGEX_LINKAGE Matches match(const Compiled& re, std::string_view s) {
  return _one_row(re, re.match(s));
}
CULEBRA_RT_REGEX_LINKAGE Matches find_all(const Compiled& re,
                                          std::string_view s) {
  auto list = re.find_all(s);
  Matches out;
  out.stride = re.group_count() + 1;
  out.spans.reserve(list.size() * out.stride);
  for (auto m : list) _push_row(out, m);
  return out;
}
// Whole-match spans only: what split / find_all_str / find_all_index need.
CULEBRA_RT_REGEX_LINKAGE std::vector<Span> find_all_spans(const Compiled& re,
                                                          std::string_view s) {
  auto list = re.find_all(s);
  std::vector<Span> out;
  out.reserve(list.size());
  for (auto m : list) out.push_back({m.begin(), m.end(), true});
  return out;
}
CULEBRA_RT_REGEX_LINKAGE size_t count(const Compiled& re, std::string_view s) {
  return re.find_all(s).size();
}
CULEBRA_RT_REGEX_LINKAGE FindAt find_at(const Compiled& re, std::string_view s,
                                        size_t pos) {
  auto r = re.find_at(s, pos);
  return {_one_row(re, r.m), r.next_pos};
}
CULEBRA_RT_REGEX_LINKAGE std::string replace_all(const Compiled& re,
                                                 std::string_view s,
                                                 std::string_view repl) {
  return re.replace_all(s, repl);
}
CULEBRA_RT_REGEX_LINKAGE std::string replace_first(const Compiled& re,
                                                   std::string_view s,
                                                   std::string_view repl) {
  return re.replace_first(s, repl);
}

#endif  // CULEBRA_RT_REGEX_WEAK

}  // namespace culebra::regex
