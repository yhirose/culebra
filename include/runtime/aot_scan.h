#pragma once

// Static analysis of which stdlib namespaces a program references.
// Used by `culebra build` to decide which optional system libraries
// (e.g. Accelerate / BLAS for Tensor) must be linked against the
// final binary. Conservative — false positives only inflate the
// binary; false negatives would leave runtime references unresolved.

#include <parser.h>

namespace culebra {

struct AotNamespaceUsage {
  bool math = false;
  bool io = false;
  bool random = false;
  bool sys = false;
  bool tensor = false;
  bool json = false;
};

inline void aot_scan_node(const peg::Ast& node, AotNamespaceUsage& out) {
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_) {
    auto name = node.token;
    if (name == "Math") out.math = true;
    else if (name == "IO") out.io = true;
    else if (name == "Random") out.random = true;
    else if (name == "Sys") out.sys = true;
    else if (name == "Tensor") out.tensor = true;
    else if (name == "JSON") out.json = true;
  }
  for (const auto& child : node.nodes) {
    aot_scan_node(*child, out);
  }
}

inline AotNamespaceUsage aot_scan_namespaces(const peg::Ast& ast) {
  AotNamespaceUsage usage;
  aot_scan_node(ast, usage);
  return usage;
}

}  // namespace culebra
