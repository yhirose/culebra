#pragma once

// Static analysis over a parsed program, for `culebra build`:
//   * which stdlib namespaces it references — each AOT feature axis force-loads
//     a runtime archive and appends its link flags on a hit (see kFeatureAxes
//     in src/main.cc for the axes themselves)
//   * which directories it bakes in via `Embed.dir("...")`

#include <parser.h>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace culebra {

// Extract a compile-time-constant string from a literal AST node: a raw STRING
// (single-quote / backtick) or a non-interpolated double-quoted string. Returns
// false for anything computed (an interpolation expression, a call, …) — those
// can't be baked at build time.
inline bool _ast_literal_string(const peg::Ast& n, std::string& out) {
  using namespace peg::udl;
  if (n.tag == "STRING"_) {
    out = std::string(n.token);
    return true;
  }
  if (n.tag == "INTERPOLATED_CONTENT"_) {
    out = decode_interpolated_content(n.token);
    return true;
  }
  if (n.tag == "INTERPOLATED_STRING"_) {
    std::string s;
    for (const auto& c : n.nodes) {
      if (c->tag != "INTERPOLATED_CONTENT"_) return false;  // has interpolation
      s += decode_interpolated_content(c->token);
    }
    out = std::move(s);
    return true;
  }
  return false;
}

// The constant string literal of `Embed.dir("...")`'s argument, descending only
// through structural wrappers — never into a nested call. So `Embed.dir("ui")`
// reads "ui", but `Embed.dir(make_path("ui"))` finds no literal (returns false)
// rather than baking the wrong directory from a buried argument. Returns false
// when the argument isn't a plain string literal (the runtime then falls back
// to live disk, and nothing is embedded).
inline bool _first_literal_string(const peg::Ast& node, std::string& out) {
  using namespace peg::udl;
  if (_ast_literal_string(node, out)) return true;
  if (node.tag == "CALL"_) return false;  // don't cross into a nested call
  for (const auto& c : node.nodes)
    if (_first_literal_string(*c, out)) return true;
  return false;
}

// Collect the literal directory names passed to `Embed.dir("...")` across the
// program. `culebra build` walks each at build time (relative to the entry
// script) and bakes its files into the binary; dirs whose argument isn't a
// string literal are skipped (they fall back to live disk at runtime). The
// returned names are deduplicated.
inline void aot_collect_embed_dirs(const peg::Ast& node,
                                   std::vector<std::string>& out) {
  using namespace peg::udl;
  if (node.tag == "CALL"_) {
    for (size_t i = 1; i + 1 < node.nodes.size(); i++) {
      const auto& dot = *node.nodes[i];
      const auto& recv = *node.nodes[i - 1];
      if ((dot.original_tag == "DOT"_) && dot.token == "dir" &&
          recv.tag == "IDENTIFIER"_ && recv.original_tag != "DOT"_ &&
          recv.token == "Embed" &&
          node.nodes[i + 1]->original_tag == "ARGUMENTS"_) {
        std::string name;
        if (_first_literal_string(*node.nodes[i + 1], name) &&
            std::find(out.begin(), out.end(), name) == out.end()) {
          out.push_back(name);
        }
      }
    }
  }
  for (const auto& child : node.nodes) aot_collect_embed_dirs(*child, out);
}

// Does the program reference any of `names` as a bare identifier? This is the
// one scan behind every AOT feature axis, and behind `culebra wrap`'s namespace
// set — which is why it takes the names rather than hardcoding them: the wrap
// axis learns its namespaces from the driver's own registry at runtime.
//
// The match is deliberately conservative and purely textual, so
// `Tensor.zeros(...)` style references and any bare `Tensor` identifier alike
// flip the flag. A false positive only inflates the binary; a false negative
// leaves runtime calls unresolved (or a wrapped namespace unregistered), so
// over-matching is the safe direction. `names` holds one or two entries per
// axis, so the linear scan per identifier is fine.
inline bool aot_uses_any_name(const peg::Ast& node,
                              std::span<const std::string_view> names) {
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_) {
    for (auto n : names)
      if (node.token == n) return true;
  }
  for (const auto& child : node.nodes) {
    if (aot_uses_any_name(*child, names)) return true;
  }
  return false;
}

}  // namespace culebra
