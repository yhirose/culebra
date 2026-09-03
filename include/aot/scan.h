#pragma once

// Static analysis over a parsed program, for `culebra build`:
//   * which stdlib namespaces it references — each AOT feature axis force-loads
//     a runtime archive and appends its link flags on a hit (see kFeatureAxes
//     in src/main.cc for the axes themselves)
//   * which directories it bakes in via `Embed.dir("...")`

#include <frontend/parser.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
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

// Every identifier the program names — the one scan behind every AOT usage
// question: a feature axis, `culebra wrap`'s namespaces (learned from the
// driver's own registry at runtime, hence a set and not a hardcoded list) and
// the stdlib namespace groups. Collected once over every module, the spliced
// preamble included, and each question is then a lookup (aot_named).
//
// The match is deliberately conservative and purely textual, so
// `Tensor.zeros(...)` style references and any bare `Tensor` identifier alike
// count. A false positive only inflates the binary; a false negative leaves
// runtime calls unresolved (or a wrapped namespace unregistered), so
// over-matching is the safe direction. `_X` is the native namespace behind
// stdlib `X` (`_Canvas`, `_Regex`, …), which the preamble and the tests call
// directly, so it counts as a use of `X`: the set holds the stripped spelling.
using AotNames = std::unordered_set<std::string_view>;

inline std::string_view _aot_name_key(std::string_view id) {
  if (id.starts_with('_')) id.remove_prefix(1);
  return id;
}

inline void aot_collect_names(const peg::Ast& node, AotNames& out) {
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_) out.insert(_aot_name_key(node.token));
  for (const auto& child : node.nodes) aot_collect_names(*child, out);
}

// Does the program name `name` (either spelling, `X` or `_X`)?
inline bool aot_named(const AotNames& names, std::string_view name) {
  return names.contains(_aot_name_key(name));
}

}  // namespace culebra
