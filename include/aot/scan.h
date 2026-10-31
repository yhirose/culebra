#pragma once

// Static analysis over a parsed program, for `culebra build`:
//   * which stdlib namespaces it references — each AOT feature axis force-loads
//     a runtime archive and appends its link flags on a hit (see kFeatureAxes
//     in src/main.cc for the axes themselves)
//   * which directories it bakes in via `Dir.embedded("...")`

#include <frontend/embed_paths.h>
#include <frontend/module_loader.h>  // LoadedModule, is_library_path
#include <frontend/parser.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace culebra {

// Collect the literal directory names passed to `Dir.embedded("...")` across the
// program. `culebra build` walks each at build time (relative to the entry
// script: the loader has made every other module's names so,
// frontend/embed_paths.h) and bakes its files into the binary; dirs whose
// argument isn't a string literal are skipped (they fall back to live disk at
// runtime). The returned names are deduplicated.
inline void aot_collect_embed_dirs(const peg::Ast& node,
                                   std::vector<std::string>& out) {
  for_each_embedded_call(node, [&](const peg::Ast& call, size_t i) {
    std::string name;
    if (auto* slot = embedded_literal_slot(*call.nodes[i + 1]);
        slot && _ast_literal_string(**slot, name) &&
        std::find(out.begin(), out.end(), name) == out.end())
      out.push_back(name);
  });
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

// The traits the library declares — the core built-in ones, which run in
// every program but are no module the loader returns, and the units the
// stdlib splice declares (Dir) — reach a namespace only through a default
// body (`Dir.copy_to`'s `FS`). A default runs only when something calls it
// by name, so its body counts as named exactly when its method's name is;
// run to a fixpoint, since a default the program calls may call another.
// The declarations' own names never count (the trait `Dir` is not a use of
// the namespace `_Dir`).
inline void _aot_collect_trait_methods(const peg::Ast& node,
                                       std::vector<const peg::Ast*>& out) {
  using namespace peg::udl;
  if (node.tag == "TRAIT_METHOD"_) {
    out.push_back(&node);
    return;
  }
  for (const auto& child : node.nodes) _aot_collect_trait_methods(*child, out);
}
// aot_collect_names over a library module, its trait declarations held back.
inline void _aot_collect_library_names(const peg::Ast& node, AotNames& out,
                                       std::vector<const peg::Ast*>& held) {
  using namespace peg::udl;
  if (node.tag == "TRAIT_DECL"_) {
    _aot_collect_trait_methods(node, held);
    return;
  }
  if (node.tag == "IDENTIFIER"_) out.insert(_aot_name_key(node.token));
  for (const auto& child : node.nodes)
    _aot_collect_library_names(*child, out, held);
}

// Every name a program's module list reaches, the spliced preamble included.
inline AotNames aot_collect_program_names(
    const std::vector<LoadedModule>& modules) {
  using namespace peg::udl;
  AotNames out;
  std::vector<const peg::Ast*> methods;
  for (const auto& m : modules) {
    if (!m.ast) continue;
    if (is_library_path(m.abs_path.string()))
      _aot_collect_library_names(*m.ast, out, methods);
    else
      aot_collect_names(*m.ast, out);
  }
  if (auto ast = parse_builtin_traits_preamble())
    _aot_collect_trait_methods(*ast, methods);
  std::vector<bool> taken(methods.size(), false);
  for (bool grew = true; grew;) {
    grew = false;
    for (size_t i = 0; i < methods.size(); i++) {
      const auto& m = *methods[i];
      if (taken[i] || m.nodes.empty() || !aot_named(out, m.nodes[0]->token))
        continue;
      taken[i] = grew = true;
      for (const auto& part : m.nodes)
        if (part->tag == "TRAIT_BODY"_) aot_collect_names(*part, out);
    }
  }
  return out;
}

}  // namespace culebra
